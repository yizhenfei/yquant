#include "tradingd/journal.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "yquant/base/format.h"

namespace yquant::tradingd {
namespace {

class JournalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-journal-XXXXXX";
    ASSERT_NE(::mkdtemp(path), nullptr);
    directory_ = path;
    options_.directory = directory_.c_str();
    options_.segment_bytes = 65536;
    options_.sync_after_bytes = 4096;
  }
  void TearDown() override {
    std::error_code error;
    ::yquant::testing::files::RemoveTree(
        ::yquant::testing::files::FilePath(directory_), error);
    EXPECT_FALSE(error);
  }
  std::string Segment(unsigned sequence = 0, unsigned stream = 1) const {
    char suffix[128];
    ::yquant::base::FormatTo(suffix, sizeof(suffix),
                             "/stream-{:020}/segment-{:020}.wal", stream,
                             sequence);
    return directory_ + suffix;
  }
  void ChangeByte(const std::string& path, off_t offset) {
    const auto fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    const std::byte bad{0xff};
    EXPECT_EQ(::pwrite(fd, &bad, 1, offset), 1);
    EXPECT_EQ(::close(fd), 0);
  }
  static base::Status Collect(void* context, uint16_t type, uint64_t lsn,
                              std::span<const std::byte> payload) noexcept {
    auto& records = *static_cast<std::vector<uint64_t>*>(context);
    if (type != 7 || payload.empty()) {
      return {base::ErrorCode::kCorrupt, {}};
    }
    records.push_back(lsn);
    return {};
  }
  std::string directory_;
  JournalOptions options_{};
};

TEST_F(JournalTest, AppendsReplaysAndLeavesUnusedPreallocatedTail) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok()) << journal.error_detail();
  const std::array payload{std::byte{1}, std::byte{2}};
  uint64_t lsn = 42;
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  EXPECT_EQ(lsn, 1);
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  EXPECT_EQ(lsn, 2);
  EXPECT_EQ(journal.published_lsn(), 2);
  ASSERT_TRUE(journal.Flush().ok());
  EXPECT_EQ(journal.durable_lsn(), 2);
  ASSERT_TRUE(journal.Close().ok());
  EXPECT_EQ(::yquant::testing::files::FileSize(
                ::yquant::testing::files::FilePath(Segment())),
            65536);

  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_EQ(records, (std::vector<uint64_t>{1, 2}));
  EXPECT_EQ(result.last_lsn, 2);
  EXPECT_FALSE(result.tail_discarded);
  ASSERT_TRUE(reader.Start(2).ok());
  ASSERT_TRUE(reader.TryAppend(7, payload, lsn).ok());
  EXPECT_EQ(lsn, 1);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest,
       ProcessExitRecoversPublishedPagesWithoutFlushAcknowledgment) {
  options_.sync_interval_ms = 1000;
  options_.sync_after_bytes = options_.segment_bytes;
  const auto child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    Journal journal;
    if (!journal.Acquire(options_).ok() || !journal.Start(1).ok()) {
      ::_exit(1);
    }
    const std::array<std::byte, 4> payload{};
    uint64_t lsn = 0;
    if (!journal.TryAppend(7, payload, lsn).ok() || lsn != 1 ||
        journal.durable_lsn() != 0) {
      ::_exit(2);
    }
    // No destructors, Close, msync or fsync for the published record.
    ::_exit(0);
  }
  int exit_status = 0;
  ASSERT_EQ(::waitpid(child, &exit_status, 0), child);
  ASSERT_TRUE(WIFEXITED(exit_status));
  ASSERT_EQ(WEXITSTATUS(exit_status), 0);
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_EQ(records, (std::vector<uint64_t>{1}));
  EXPECT_FALSE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, RotatesAcrossSealedSegmentsAndKeepsLsnContinuous) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  uint64_t lsn = 0;
  for (uint64_t expected = 1; expected <= 100; ++expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    base::Status status;
    do {
      status = journal.TryAppend(7, payload, lsn);
      if (status.ok() || !journal.healthy()) {
        break;
      }
      ASSERT_EQ(status.code, base::ErrorCode::kResourceExhausted);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_TRUE(status.ok()) << journal.error_detail();
    ASSERT_EQ(lsn, expected);
  }
  ASSERT_TRUE(journal.Close().ok());
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_EQ(records.size(), 100);
  EXPECT_EQ(result.last_lsn, 100);
  EXPECT_FALSE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, CheckpointCursorIgnoresDamagedCoveredRecordsAndSegments) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  uint64_t lsn = 0;
  for (unsigned i = 0; i < 4; ++i) {
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  }
  const auto cursor = journal.GetCursor();
  ASSERT_EQ(cursor.segment, 1);
  ASSERT_EQ(cursor.lsn, 4);
  for (unsigned i = 0; i < 4; ++i) {
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  }
  ASSERT_TRUE(journal.Close().ok());
  ChangeByte(Segment(0), 16);       // Covered segment header.
  ChangeByte(Segment(1), 64 + 40);  // Covered record in the anchor segment.
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.ReplayFrom(cursor, Collect, &records, result).ok());
  EXPECT_EQ(records, (std::vector<uint64_t>{5, 6, 7, 8}));
  EXPECT_EQ(result.records, 4);
  EXPECT_EQ(result.last_lsn, 8);
  EXPECT_FALSE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest,
       CheckpointCursorAtFutureSegmentEndContinuesIntoNextSegment) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  uint64_t lsn = 0;
  for (unsigned i = 0; i < 3; ++i) {
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  }
  const auto cursor = journal.GetCursor();
  ASSERT_EQ(cursor.segment, 0);
  ASSERT_EQ(cursor.lsn, 3);
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Close().ok());
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.ReplayFrom(cursor, Collect, &records, result).ok());
  EXPECT_EQ(records, (std::vector<uint64_t>{4}));
  EXPECT_FALSE(result.tail_discarded);
  auto invalid = cursor;
  invalid.offset = 63;
  EXPECT_EQ(reader.ReplayFrom(invalid, Collect, &records, result).code,
            base::ErrorCode::kCorrupt);
  invalid = cursor;
  invalid.offset = 65536;
  EXPECT_EQ(reader.ReplayFrom(invalid, Collect, &records, result).code,
            base::ErrorCode::kCorrupt);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, StopsAtFirstCorruptPayloadWithoutSkippingLaterRecord) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array<std::byte, 4> payload{};
  uint64_t lsn = 0;
  for (unsigned i = 0; i < 3; ++i) {
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  }
  ASSERT_TRUE(journal.Close().ok());
  // A four-byte payload occupies 56 bytes; corrupt the second payload.
  ChangeByte(Segment(), 64 + 56 + 40);
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_EQ(records, (std::vector<uint64_t>{1}));
  EXPECT_TRUE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, RejectsRecordWithTornPublicationMarker) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array<std::byte, 4> payload{};
  uint64_t lsn = 0;
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Close().ok());
  ChangeByte(Segment(), 64 + 56 - 8);
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_TRUE(records.empty());
  EXPECT_TRUE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, MissingContinuationStopsAtPriorSealedPrefix) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  uint64_t lsn = 0;
  for (unsigned i = 0; i < 4; ++i) {
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  }
  ASSERT_TRUE(journal.Close().ok());
  ASSERT_TRUE(::yquant::testing::files::RemoveFile(
      ::yquant::testing::files::FilePath(Segment(1))));
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  std::vector<uint64_t> records;
  ASSERT_TRUE(reader.Replay(1, Collect, &records, result).ok());
  EXPECT_EQ(result.last_lsn, 3);
  EXPECT_TRUE(result.tail_discarded);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, CorruptRequiredSegmentHeaderIsFatal) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  ASSERT_TRUE(journal.Close().ok());
  ChangeByte(Segment(), 16);
  Journal reader;
  ASSERT_TRUE(reader.Acquire(options_).ok());
  JournalReplayResult result;
  result.last_lsn = 99;
  std::vector<uint64_t> records;
  EXPECT_EQ(reader.Replay(1, Collect, &records, result).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(result.last_lsn, 99);
  EXPECT_TRUE(reader.Close().ok());
}

TEST_F(JournalTest, DirectoryLockCoversRecoveryAndIsNeverReplaced) {
  Journal owner;
  ASSERT_TRUE(owner.Acquire(options_).ok());
  const std::array<std::byte, 1> payload{};
  EXPECT_EQ(owner.SaveFile("LOCK", payload).code,
            base::ErrorCode::kInvalidArgument);
  Journal other;
  EXPECT_FALSE(other.Acquire(options_).ok());
  ASSERT_TRUE(owner.Close().ok());
  ASSERT_TRUE(other.Acquire(options_).ok());
  EXPECT_TRUE(other.Close().ok());
}

struct SlowFlush {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  ~SlowFlush() { release.store(true, std::memory_order_release); }
  static int Flush(void* context, int fd, void* mapping,
                   size_t bytes) noexcept {
    auto& self = *static_cast<SlowFlush*>(context);
    self.entered.store(true, std::memory_order_release);
    while (!self.release.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (::msync(mapping, bytes, MS_SYNC) != 0) {
      return errno;
    }
    return ::fsync(fd) == 0 ? 0 : errno;
  }
};

TEST_F(JournalTest, AppendAndRotationDoNotWaitForBackgroundFlush) {
  SlowFlush slow;
  Journal journal;
  struct ReleaseOnExit {
    SlowFlush& slow;
    ~ReleaseOnExit() { slow.release.store(true, std::memory_order_release); }
  } release_on_exit{slow};
  const JournalIoHooks hooks{&slow, SlowFlush::Flush};
  ASSERT_TRUE(journal.Acquire(options_, &hooks).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  uint64_t lsn = 0;
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!slow.entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool entered = slow.entered.load(std::memory_order_acquire);
  // Release before an assertion can destroy a still-blocked worker.
  if (!entered) {
    slow.release.store(true, std::memory_order_release);
  }
  ASSERT_TRUE(entered);
  for (unsigned i = 0; i < 8; ++i) {
    const auto status = journal.TryAppend(7, payload, lsn);
    if (!status.ok()) {
      slow.release.store(true, std::memory_order_release);
    }
    ASSERT_TRUE(status.ok());
  }
  EXPECT_EQ(lsn, 9);
  EXPECT_EQ(journal.durable_lsn(), 0);
  EXPECT_EQ(journal.TryAppend(7, payload, lsn).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(lsn, 9);
  EXPECT_TRUE(journal.healthy());
  slow.release.store(true, std::memory_order_release);
  ASSERT_TRUE(journal.Flush().ok());
  EXPECT_EQ(journal.durable_lsn(), 9);
  EXPECT_TRUE(journal.Close().ok());
}

TEST_F(JournalTest, BackgroundFailureIsRetainedAndStopsPublication) {
  Journal journal;
  const JournalIoHooks hooks{
      nullptr, [](void*, int, void*, size_t) noexcept { return EIO; }};
  ASSERT_TRUE(journal.Acquire(options_, &hooks).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array<std::byte, 4096> payload{};
  uint64_t lsn = 0;
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  EXPECT_EQ(journal.Flush().code, base::ErrorCode::kIoError);
  EXPECT_FALSE(journal.healthy());
  EXPECT_EQ(journal.TryAppend(7, payload, lsn).code, base::ErrorCode::kIoError);
  EXPECT_EQ(lsn, 1);
  EXPECT_EQ(journal.published_lsn(), 1);
  EXPECT_EQ(journal.durable_lsn(), 0);
  EXPECT_EQ(journal.Close().code, base::ErrorCode::kIoError);
  EXPECT_EQ(journal.Close().code, base::ErrorCode::kIoError);
}

TEST_F(JournalTest, CheckedFilesReplaceAtomicallyAndDetectCorruption) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  const std::array first{std::byte{1}, std::byte{2}, std::byte{3}};
  const std::array second{std::byte{4}, std::byte{5}};
  ASSERT_TRUE(journal.SaveFile("IDENTITY", first).ok());
  ASSERT_TRUE(journal.SaveFile("IDENTITY", second).ok());
  std::array<std::byte, 8> storage{};
  size_t size = 99;
  ASSERT_TRUE(journal.LoadFile("IDENTITY", storage, size).ok());
  EXPECT_EQ(size, 2);
  EXPECT_EQ(storage[0], std::byte{4});
  EXPECT_EQ(storage[1], std::byte{5});
  EXPECT_EQ(journal.SaveFile("../escape", first).code,
            base::ErrorCode::kInvalidArgument);
  ChangeByte(directory_ + "/IDENTITY", 64);
  size = 99;
  EXPECT_EQ(journal.LoadFile("IDENTITY", storage, size).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(size, 99);
  EXPECT_TRUE(journal.Close().ok());
}

TEST_F(JournalTest, RejectsInvalidOptionsAndOversizedTransactions) {
  Journal journal;
  auto invalid = options_;
  invalid.reserve_segments = 1;
  EXPECT_EQ(journal.Acquire(invalid).code, base::ErrorCode::kInvalidArgument);
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  uint64_t lsn = 99;
  std::array<std::byte, kJournalMaxPayloadBytes + 1> payload{};
  EXPECT_EQ(journal.TryAppend(7, payload, lsn).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(journal.TryAppend(0, {}, lsn).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(lsn, 99);
  EXPECT_EQ(journal.published_lsn(), 0);
  EXPECT_TRUE(journal.Close().ok());
}

}  // namespace
}  // namespace yquant::tradingd
