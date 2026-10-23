#include "yquant/base/shm_broadcast.h"

#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <span>
#include <system_error>
#include <thread>

#include "base/internal/broadcast_layout.h"
#include "base/internal/shared_memory_mapping.h"
#include "base/internal/shm_broadcast_layout.h"
#include "base/internal/shm_stream_registry.h"
#include "file_test_support.h"
#include "gtest/gtest.h"

namespace yquant::base {
namespace {
class Pipe {
 public:
  Pipe() { EXPECT_EQ(::pipe(fds_), 0); }
  ~Pipe() {
    ::close(fds_[0]);
    ::close(fds_[1]);
  }
  void Signal() {
    const char byte = 1;
    ASSERT_EQ(::write(fds_[1], &byte, 1), 1);
  }
  void Wait() {
    pollfd descriptor{fds_[0], POLLIN, 0};
    ASSERT_EQ(::poll(&descriptor, 1, 5000), 1);
    char byte;
    ASSERT_EQ(::read(fds_[0], &byte, 1), 1);
  }

 private:
  int fds_[2] = {-1, -1};
};

// Child-side GoogleTest failures become a nonzero exit status in the parent.
class Child {
 public:
  template <typename Function>
  explicit Child(Function function) {
    pid_ = ::fork();
    EXPECT_GE(pid_, 0);
    if (pid_ == 0) {
      function();
      ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    }
  }
  ~Child() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  void Kill() { ASSERT_EQ(::kill(pid_, SIGKILL), 0); }
  int Wait() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return status;
      }
      if (result < 0 && errno != EINTR) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ADD_FAILURE() << "child wait timed out";
    return -1;
  }
  void Success() {
    const auto status = Wait();
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

 private:
  pid_t pid_ = -1;
};

class ShmByteBroadcastTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(::mkdtemp(directory_), nullptr);
    endpoint_ = {directory_, "bytes"};
  }
  void TearDown() override {
    auto stale = internal::Discover(endpoint_);
    if (stale) {
      ::shm_unlink(stale->shm_name.c_str());
    }
    std::error_code error;
    ::yquant::testing::files::RemoveTree(
        ::yquant::testing::files::FilePath(directory_), error);
  }
  char directory_[64] = "/tmp/yquant-byte-broadcast-XXXXXX";
  ShmEndpoint endpoint_;
};

TEST_F(ShmByteBroadcastTest, RawRecordsAboveMarketLimitAndFinalDrain) {
  ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {2, 8193}}).ok());
  EXPECT_EQ(writer.max_record_size(), 8193U);
  ShmBroadcastReader reader;
  std::array<std::byte, 8193> record{}, scratch{};
  record.fill(std::byte{0x71});
  ASSERT_TRUE(reader.Open(endpoint_, scratch).ok());
  ASSERT_TRUE(writer.TryWrite(record).ok());
  writer.Close();
  auto result = reader.TryRead();
  ASSERT_FALSE(result.stream_changed);
  ASSERT_EQ(result.read.status, BroadcastReadStatus::kRecord);
  ASSERT_EQ(result.read.record.size(), 8193U);
  EXPECT_TRUE(
      std::equal(record.begin(), record.end(), result.read.record.begin()));
  EXPECT_EQ(reader.TryRead().read.status, BroadcastReadStatus::kEnd);
}

TEST_F(ShmByteBroadcastTest, AtomicsAliasAcrossMappingsAndProcesses) {
  ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {8, 32}}).ok());
  auto manifest = internal::Discover(endpoint_);
  ASSERT_TRUE(manifest);
  internal::SharedMemoryMapping first, second;
  ASSERT_TRUE(first.Open(manifest->shm_name).ok());
  ASSERT_TRUE(second.Open(manifest->shm_name).ok());
  const auto word =
      [](internal::SharedMemoryMapping& mapping) -> internal::AtomicWord& {
    auto memory =
        mapping.memory().subspan(sizeof(internal::ShmBroadcastControl));
    auto* header = reinterpret_cast<internal::BroadcastHeader*>(memory.data());
    const auto& layout = header->layout;
    return internal::BroadcastSlot(memory.data() + layout.slots_offset,
                                   layout.slot_stride, layout.capacity, 1, 2);
  };
  EXPECT_NE(&word(first), &word(second));
  word(first).store(0x123456789abcdef0ULL, std::memory_order_release);
  EXPECT_EQ(word(second).load(std::memory_order_acquire),
            0x123456789abcdef0ULL);
  Child child([&] {
    internal::SharedMemoryMapping alias;
    ASSERT_TRUE(alias.Open(manifest->shm_name).ok());
    EXPECT_NE(&word(alias), &word(first));
    EXPECT_EQ(word(alias).load(std::memory_order_acquire),
              0x123456789abcdef0ULL);
    word(alias).store(0xfedcba9876543210ULL, std::memory_order_release);
  });
  child.Success();
  EXPECT_EQ(word(first).load(std::memory_order_acquire), 0xfedcba9876543210ULL);
}

TEST_F(ShmByteBroadcastTest, ValidatesEnvelopeAndQueueIndependently) {
  ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {8, 32}}).ok());
  auto manifest = internal::Discover(endpoint_);
  ASSERT_TRUE(manifest);
  internal::SharedMemoryMapping mapping;
  ASSERT_TRUE(mapping.Open(manifest->shm_name).ok());
  auto* control =
      reinterpret_cast<internal::ShmBroadcastControl*>(mapping.memory().data());
  auto* queue = reinterpret_cast<internal::BroadcastHeader*>(
      mapping.memory().data() + sizeof(*control));
  const auto envelope = control->layout;
  const auto layout = queue->layout;
  std::array<std::byte, 32> scratch;
  const auto rejected = [&] {
    ShmBroadcastReader reader;
    EXPECT_EQ(reader.Open(endpoint_, scratch).code, ErrorCode::kCorrupt);
    control->layout = envelope;
    queue->layout = layout;
  };
  control->layout.heartbeat_offset += 1;
  rejected();
  control->layout.bytes += 4096;
  rejected();
  control->layout.queue_offset = 1;
  rejected();
  control->layout.queue_bytes = ~uint64_t{};
  rejected();
  control->layout.generation = {9, 9};
  rejected();
  queue->layout.abi ^= 1;
  rejected();
  queue->layout.slot_stride = ~uint64_t{};
  rejected();
  queue->layout.generation = {9, 9};
  rejected();
  ASSERT_EQ(::shm_unlink(manifest->shm_name.c_str()), 0);
  const int fd =
      ::shm_open(manifest->shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  rejected();
  EXPECT_EQ(::ftruncate(fd, 8), 0);
  ::close(fd);
  rejected();
}

TEST_F(ShmByteBroadcastTest,
       ReplacementIsDiscoveryStateAndInvalidatesGapTokens) {
  ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {2, 32}}).ok());
  const auto generation = writer.generation();
  ShmBroadcastReader reader;
  std::array<std::byte, 32> scratch;
  ASSERT_TRUE(reader.Open(endpoint_, scratch).ok());
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.TryWrite({}).ok());
  }
  auto gap = reader.TryRead();
  ASSERT_TRUE(gap.read.gap);
  writer.Close();
  ASSERT_TRUE(writer.Create({endpoint_, {2, 32}}).ok());
  EXPECT_NE(writer.generation(), generation);
  ShmRefreshResult refresh;
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, ShmRefreshStatus::kStreamChanged);
  EXPECT_TRUE(reader.TryRead().stream_changed);
  uint64_t sequence = 99;
  EXPECT_FALSE(reader.Resume(*gap.read.gap, sequence).ok());
  EXPECT_EQ(sequence, 99U);
}

TEST_F(ShmByteBroadcastTest, ReaderTokensDifferAcrossForkChildren) {
  ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {2, 32}}).ok());
  for (int i = 0; i < 4; ++i) {
    ASSERT_TRUE(writer.TryWrite({}).ok());
  }
  int descriptors[2];
  ASSERT_EQ(::pipe(descriptors), 0);
  const auto send_token = [&] {
    ShmBroadcastReader reader;
    std::array<std::byte, 32> scratch;
    ASSERT_TRUE(
        reader.Open(endpoint_, scratch, BroadcastStartPosition::At(1)).ok());
    auto result = reader.TryRead();
    ASSERT_TRUE(result.read.gap);
    ASSERT_EQ(
        ::write(descriptors[1], &*result.read.gap, sizeof(BroadcastGapToken)),
        static_cast<ssize_t>(sizeof(BroadcastGapToken)));
  };
  Child first(send_token);
  first.Success();
  Child second(send_token);
  second.Success();
  BroadcastGapToken a, b;
  EXPECT_EQ(::read(descriptors[0], &a, sizeof(a)),
            static_cast<ssize_t>(sizeof(a)));
  EXPECT_EQ(::read(descriptors[0], &b, sizeof(b)),
            static_cast<ssize_t>(sizeof(b)));
  EXPECT_EQ(a.generation, b.generation);
  EXPECT_EQ(a.expected_sequence, b.expected_sequence);
  EXPECT_NE(a.reader_instance, b.reader_instance);
  ::close(descriptors[0]);
  ::close(descriptors[1]);
}

TEST_F(ShmByteBroadcastTest, FailureDoesNotPublishAndAllowsRetry) {
  ShmBroadcastWriter first, duplicate;
  EXPECT_FALSE(first.Create({endpoint_, {3, 32}}).ok());
  EXPECT_FALSE(first.Create({endpoint_, {2, size_t{1} << 30}}).ok());
  EXPECT_FALSE(internal::Discover(endpoint_));
  ASSERT_TRUE(first.Create({endpoint_, {2, 32}}).ok());
  EXPECT_FALSE(duplicate.Create({endpoint_, {2, 32}}).ok());
  first.Close();
  ASSERT_TRUE(duplicate.Create({endpoint_, {2, 32}}).ok());
  ShmBroadcastReader reader;
  std::array<std::byte, 32> scratch;
  EXPECT_FALSE(reader.Open(endpoint_, std::span(scratch).first(31)).ok());
  ASSERT_TRUE(reader.Open(endpoint_, scratch).ok());
  ShmRefreshResult refresh;
  refresh.health = ShmWriterHealth::kStale;
  EXPECT_FALSE(reader.Refresh(refresh, -std::chrono::nanoseconds(1)).ok());
  EXPECT_EQ(refresh.health, ShmWriterHealth::kStale);
}
}  // namespace
}  // namespace yquant::base
