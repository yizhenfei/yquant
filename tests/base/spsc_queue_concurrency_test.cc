#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>

#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;
using Clock = std::chrono::steady_clock;
constexpr auto kWorkerTimeout = std::chrono::seconds(15);
constexpr uint64_t kThreadFrameCount = 50000;

size_t MessageSize(uint64_t sequence) { return sequence % 129; }

std::byte MessageByte(uint64_t sequence, size_t offset) {
  return static_cast<std::byte>((sequence * 131 + offset * 17) & 0xff);
}

void FillMessage(std::span<std::byte> payload, uint64_t sequence) {
  for (size_t index = 0; index < payload.size(); ++index) {
    payload[index] = MessageByte(sequence, index);
  }
}

bool CheckMessage(std::span<const std::byte> payload, uint64_t sequence) {
  if (payload.size() != MessageSize(sequence)) {
    return false;
  }
  for (size_t index = 0; index < payload.size(); ++index) {
    if (payload[index] != MessageByte(sequence, index)) {
      return false;
    }
  }
  return true;
}

// No GoogleTest operations run on worker threads.
// Every retry loop checks both a local deadline and the shared stop request.
struct Worker {
  std::span<std::byte> memory;
  uint64_t frame_count;
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
  for (uint64_t sequence = 0; sequence < worker.frame_count;) {
    if (worker.stop->load(std::memory_order_relaxed) ||
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

void ExpectConcurrentTransfers(std::span<std::byte> memory, bool zero_copy) {
  std::atomic<bool> stop{false};
  std::atomic<int> failure{0};
  Worker producer{memory, kThreadFrameCount, true, zero_copy, &stop, &failure};
  Worker consumer{memory, kThreadFrameCount, false, zero_copy, &stop, &failure};
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
  ExpectConcurrentTransfers(memory, true);
}

TEST(SpscQueueConcurrencyTest, TwoThreadsTransferCopiedFrames) {
  alignas(128) std::array<std::byte, 1408> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 1024), SpscStatus::kSuccess);
  ExpectConcurrentTransfers(memory, false);
}

}  // namespace
}  // namespace yquant::base
