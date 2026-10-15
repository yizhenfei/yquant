#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;
using Clock = std::chrono::steady_clock;
constexpr auto kWorkerTimeout = std::chrono::seconds(15);
constexpr std::uint64_t kThreadFrameCount = 50000;
constexpr std::uint64_t kProcessFrameCount = 20000;

std::size_t MessageSize(std::uint64_t sequence) { return sequence % 129; }

std::byte MessageByte(std::uint64_t sequence, std::size_t offset) {
  return static_cast<std::byte>((sequence * 131 + offset * 17) & 0xff);
}

void FillMessage(std::span<std::byte> payload, std::uint64_t sequence) {
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = MessageByte(sequence, index);
  }
}

bool CheckMessage(std::span<const std::byte> payload, std::uint64_t sequence) {
  if (payload.size() != MessageSize(sequence)) {
    return false;
  }
  for (std::size_t index = 0; index < payload.size(); ++index) {
    if (payload[index] != MessageByte(sequence, index)) {
      return false;
    }
  }
  return true;
}

// No GoogleTest operations run on worker threads or in the forked child.
// Every retry loop checks both a local deadline and the shared stop request.
struct Worker {
  std::span<std::byte> memory;
  std::uint64_t frame_count;
  bool producer;
  bool zero_copy;
  std::atomic<bool>* stop;
  std::atomic<int>* failure;
};

int RunWorker(const Worker& worker) {
  SpscQueue queue;
  const auto role = worker.producer ? Role::kProducer : Role::kConsumer;
  if (queue.Attach(worker.memory, role) != SpscStatus::kSuccess) {
    return 1;
  }
  const auto deadline = Clock::now() + kWorkerTimeout;
  std::array<std::byte, 128> buffer{};
  for (std::uint64_t sequence = 0; sequence < worker.frame_count;) {
    if ((worker.stop != nullptr &&
         worker.stop->load(std::memory_order_relaxed)) ||
        Clock::now() >= deadline) {
      return 2;
    }
    if (worker.producer) {
      const auto size = MessageSize(sequence);
      SpscStatus status;
      if (worker.zero_copy) {
        auto result = queue.TryReserve(size);
        status = result.status;
        if (status == SpscStatus::kSuccess) {
          FillMessage(result.reservation.payload(), sequence);
          if (result.reservation.Commit(size) != SpscStatus::kSuccess) {
            return 3;
          }
        }
      } else {
        auto payload = std::span(buffer).first(size);
        FillMessage(payload, sequence);
        status = queue.TryWrite(payload);
      }
      if (status == SpscStatus::kFull) {
        sched_yield();
        continue;
      }
      if (status != SpscStatus::kSuccess) {
        return 4;
      }
    } else if (worker.zero_copy) {
      auto result = queue.TryAcquire();
      if (result.status == SpscStatus::kEmpty) {
        sched_yield();
        continue;
      }
      if (result.status != SpscStatus::kSuccess) {
        return 5;
      }
      if (!CheckMessage(result.lease.payload(), sequence)) {
        return 6;
      }
      // Occasionally keep the read pinned while the producer is runnable.
      if ((sequence & 127) == 0) {
        sched_yield();
        if (!CheckMessage(result.lease.payload(), sequence)) {
          return 7;
        }
      }
      if (result.lease.Release() != SpscStatus::kSuccess) {
        return 8;
      }
    } else {
      auto result = queue.TryRead(buffer);
      if (result.status == SpscStatus::kEmpty) {
        sched_yield();
        continue;
      }
      if (result.status != SpscStatus::kSuccess) {
        return 9;
      }
      if (!CheckMessage(std::span(buffer).first(result.payload_size),
                        sequence)) {
        return 10;
      }
    }
    ++sequence;
  }
  return 0;
}

void* ThreadEntry(void* argument) {
  const auto& worker = *static_cast<Worker*>(argument);
  const int result = RunWorker(worker);
  if (result != 0) {
    worker.failure->store(result, std::memory_order_relaxed);
    worker.stop->store(true, std::memory_order_relaxed);
  }
  return nullptr;
}

void ExpectConcurrentTransfers(std::span<std::byte> producer_memory,
                               std::span<std::byte> consumer_memory,
                               bool zero_copy) {
  std::atomic<bool> stop{false};
  std::atomic<int> failure{0};
  Worker producer{producer_memory, kThreadFrameCount, true, zero_copy,
                  &stop,           &failure};
  Worker consumer{consumer_memory, kThreadFrameCount, false, zero_copy,
                  &stop,           &failure};
  pthread_t producer_thread;
  const int producer_result =
      pthread_create(&producer_thread, nullptr, ThreadEntry, &producer);
  ASSERT_EQ(producer_result, 0);
  pthread_t consumer_thread;
  const int consumer_result =
      pthread_create(&consumer_thread, nullptr, ThreadEntry, &consumer);
  if (consumer_result != 0) {
    stop.store(true, std::memory_order_relaxed);
    EXPECT_EQ(pthread_join(producer_thread, nullptr), 0);
    FAIL() << "pthread_create consumer: " << consumer_result;
  }
  EXPECT_EQ(pthread_join(producer_thread, nullptr), 0);
  EXPECT_EQ(pthread_join(consumer_thread, nullptr), 0);
  EXPECT_EQ(failure.load(std::memory_order_relaxed), 0);
}

TEST(SpscQueueConcurrencyTest, TwoThreadsTransferZeroCopyFrames) {
  alignas(128) std::array<std::byte, 1408> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 1024), SpscStatus::kSuccess);
  ExpectConcurrentTransfers(memory, memory, true);
}

TEST(SpscQueueConcurrencyTest, TwoThreadsTransferCopiedFrames) {
  alignas(128) std::array<std::byte, 1408> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 1024), SpscStatus::kSuccess);
  ExpectConcurrentTransfers(memory, memory, false);
}

class SharedMapping {
 public:
  SharedMapping() = default;
  ~SharedMapping() {
    if (first_ != MAP_FAILED) {
      munmap(first_, bytes_);
    }
    if (second_ != MAP_FAILED) {
      munmap(second_, bytes_);
    }
    if (file_ >= 0) {
      close(file_);
    }
  }
  SharedMapping(const SharedMapping&) = delete;
  SharedMapping& operator=(const SharedMapping&) = delete;

  bool Initialize(std::size_t bytes) {
    char path[] = "/tmp/yquant-spsc-XXXXXX";
    file_ = mkstemp(path);
    if (file_ < 0) {
      return false;
    }
    if (unlink(path) != 0 || ftruncate(file_, static_cast<off_t>(bytes)) != 0) {
      return false;
    }
    bytes_ = bytes;
    first_ =
        mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, file_, 0);
    if (first_ == MAP_FAILED) {
      return false;
    }
    second_ =
        mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, file_, 0);
    return second_ != MAP_FAILED;
  }

  std::span<std::byte> first() const {
    return {static_cast<std::byte*>(first_), bytes_};
  }
  std::span<std::byte> second() const {
    return {static_cast<std::byte*>(second_), bytes_};
  }
  bool UnmapFirst() {
    if (munmap(first_, bytes_) != 0) {
      return false;
    }
    first_ = MAP_FAILED;
    return true;
  }
  bool UnmapSecond() {
    if (munmap(second_, bytes_) != 0) {
      return false;
    }
    second_ = MAP_FAILED;
    return true;
  }

 private:
  int file_ = -1;
  void* first_ = MAP_FAILED;
  void* second_ = MAP_FAILED;
  std::size_t bytes_ = 0;
};

TEST(SpscQueueSharedMemoryTest, SeparateMappingsPublishFramesAcrossThreads) {
  SharedMapping mapping;
  ASSERT_TRUE(mapping.Initialize(1408)) << "errno: " << errno;
  ASSERT_NE(mapping.first().data(), mapping.second().data());
  ASSERT_EQ(SpscQueue::Initialize(mapping.first(), 1024), SpscStatus::kSuccess);
  ExpectConcurrentTransfers(mapping.first(), mapping.second(), true);
}

bool ReapChild(pid_t child, int* status) {
  const auto deadline = Clock::now() + kWorkerTimeout;
  while (Clock::now() < deadline) {
    const auto result = waitpid(child, status, WNOHANG);
    if (result == child) {
      return true;
    }
    if (result < 0 && errno != EINTR) {
      return false;
    }
    usleep(1000);
  }
  kill(child, SIGKILL);
  while (waitpid(child, status, 0) < 0 && errno == EINTR) {
  }
  return false;
}

bool IsThreadSanitized() {
#if defined(__SANITIZE_THREAD__)
  return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  return true;
#endif
#endif
  return false;
}

TEST(SpscQueueSharedMemoryTest, ForkedProcessesUseDifferentMappingAddresses) {
  if (IsThreadSanitized()) {
    GTEST_SKIP() << "IPC is verified in the regular and ASan builds";
  }
  SharedMapping mapping;
  ASSERT_TRUE(mapping.Initialize(1408)) << "errno: " << errno;
  ASSERT_NE(mapping.first().data(), mapping.second().data());
  ASSERT_EQ(SpscQueue::Initialize(mapping.first(), 1024), SpscStatus::kSuccess);
  // fork is the external publication barrier for initialized control objects.
  // The child only uses the second mapping; the parent only uses the first.
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    if (!mapping.UnmapFirst()) {
      _exit(11);
    }
    Worker producer{mapping.second(), kProcessFrameCount, true, true, nullptr,
                    nullptr};
    const int result = RunWorker(producer);
    _exit(result);
  }
  if (!mapping.UnmapSecond()) {
    kill(child, SIGKILL);
    int status = 0;
    ReapChild(child, &status);
    FAIL() << "munmap unused parent mapping: " << errno;
  }
  Worker consumer{mapping.first(), kProcessFrameCount, false, true, nullptr,
                  nullptr};
  const int consumer_result = RunWorker(consumer);
  if (consumer_result != 0) {
    kill(child, SIGKILL);
  }
  int status = 0;
  const bool reaped = ReapChild(child, &status);
  EXPECT_EQ(consumer_result, 0);
  ASSERT_TRUE(reaped) << "child exceeded the transfer deadline";
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
}  // namespace yquant::base
