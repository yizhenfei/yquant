#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "tradingd/journal.h"

namespace yquant::tradingd {
namespace {

using namespace std::chrono_literals;

class JournalReaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-journal-reader-XXXXXX";
    ASSERT_NE(::mkdtemp(path), nullptr);
    directory_ = path;
    options_.directory = directory_.c_str();
    options_.segment_bytes = 65536;
    options_.sync_after_bytes = 4096;
  }

  void TearDown() override {
    std::error_code error;
    std::filesystem::remove_all(directory_, error);
    EXPECT_FALSE(error);
  }

  std::string Segment(unsigned sequence = 0) const {
    char suffix[128];
    std::snprintf(suffix, sizeof(suffix), "/stream-%020u/segment-%020u.wal", 1U,
                  sequence);
    return directory_ + suffix;
  }

  void ChangeByte(const std::string& path, off_t offset) {
    const int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    const std::byte bad{0xff};
    EXPECT_EQ(::pwrite(fd, &bad, 1, offset), 1);
    EXPECT_EQ(::close(fd), 0);
  }

  static base::Status Collect(void* context, std::uint16_t type,
                              std::uint64_t lsn,
                              std::span<const std::byte> payload) noexcept {
    if (type != 7 || payload.empty() || payload.front() != std::byte{3})
      return {base::ErrorCode::kCorrupt, {}};
    auto& records = *static_cast<std::vector<std::uint64_t>*>(context);
    records.push_back(lsn);
    return {};
  }

  static void ExpectCursor(const JournalCursor& actual,
                           const JournalCursor& expected) {
    EXPECT_EQ(actual.stream_id, expected.stream_id);
    EXPECT_EQ(actual.segment, expected.segment);
    EXPECT_EQ(actual.offset, expected.offset);
    EXPECT_EQ(actual.lsn, expected.lsn);
  }

  std::string directory_;
  JournalOptions options_{};
};

TEST_F(JournalReaderTest, StopsAtBoundAndResumesWhileWriterRetainsLock) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array payload{std::byte{3}};
  std::uint64_t lsn = 0;
  for (unsigned i = 0; i < 3; ++i)
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Flush().ok());

  auto cursor = JournalStartCursor(1);
  std::vector<std::uint64_t> records;
  ASSERT_TRUE(
      ScanJournal(directory_.c_str(), cursor, 2, Collect, &records).ok());
  EXPECT_EQ(records, (std::vector<std::uint64_t>{1, 2}));
  EXPECT_EQ(cursor.segment, 0);
  EXPECT_EQ(cursor.offset, 64 + 2 * 56);
  EXPECT_EQ(cursor.lsn, 2);
  const auto at_two = cursor;
  ASSERT_TRUE(
      ScanJournal(directory_.c_str(), cursor, 2, Collect, &records).ok());
  ExpectCursor(cursor, at_two);
  ASSERT_TRUE(
      ScanJournal(directory_.c_str(), cursor, 3, Collect, &records).ok());
  EXPECT_EQ(records, (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(cursor.lsn, 3);
  EXPECT_TRUE(journal.Close().ok());
}

TEST_F(JournalReaderTest, DamagedRecordBeyondBoundDoesNotAffectEarlierBatch) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array payload{std::byte{3}};
  std::uint64_t lsn = 0;
  for (unsigned i = 0; i < 3; ++i)
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Close().ok());
  ChangeByte(Segment(), 64 + 56 + 40);

  auto cursor = JournalStartCursor(1);
  std::vector<std::uint64_t> records;
  ASSERT_TRUE(
      ScanJournal(directory_.c_str(), cursor, 1, Collect, &records).ok());
  const auto at_one = cursor;
  EXPECT_EQ(ScanJournal(directory_.c_str(), cursor, 3, Collect, &records).code,
            base::ErrorCode::kCorrupt);
  ExpectCursor(cursor, at_one);
  EXPECT_EQ(records, (std::vector<std::uint64_t>{1}));

  JournalReplayResult result;
  ASSERT_TRUE(ProbeJournal(directory_.c_str(), 1, result).ok());
  EXPECT_EQ(result.last_lsn, 1);
  EXPECT_EQ(result.records, 1);
  EXPECT_TRUE(result.tail_discarded);
}

TEST_F(JournalReaderTest,
       MissingContinuationFailsAndPreservesWholeBatchCursor) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::array<std::byte, kJournalMaxPayloadBytes> payload{};
  payload.front() = std::byte{3};
  std::uint64_t lsn = 0;
  for (unsigned i = 0; i < 4; ++i)
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Close().ok());
  ASSERT_TRUE(std::filesystem::remove(Segment(1)));

  auto cursor = JournalStartCursor(1);
  const auto start = cursor;
  std::vector<std::uint64_t> records;
  EXPECT_EQ(ScanJournal(directory_.c_str(), cursor, 4, Collect, &records).code,
            base::ErrorCode::kCorrupt);
  ExpectCursor(cursor, start);
  EXPECT_EQ(records, (std::vector<std::uint64_t>{1, 2, 3}));
  JournalReplayResult result;
  ASSERT_TRUE(ProbeJournal(directory_.c_str(), 1, result).ok());
  EXPECT_EQ(result.last_lsn, 3);
  EXPECT_TRUE(result.tail_discarded);
}

TEST_F(JournalReaderTest, ValidCrcDoesNotAllowAnLsnGap) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array payload{std::byte{3}};
  std::uint64_t lsn = 0;
  for (unsigned i = 0; i < 3; ++i)
    ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Close().ok());
  const int fd = ::open(Segment().c_str(), O_RDWR);
  ASSERT_GE(fd, 0);
  std::array<std::byte, 56> third{};
  ASSERT_EQ(::pread(fd, third.data(), third.size(), 64 + 2 * 56), 56);
  EXPECT_EQ(::pwrite(fd, third.data(), third.size(), 64 + 56), 56);
  EXPECT_EQ(::close(fd), 0);

  auto cursor = JournalStartCursor(1);
  const auto start = cursor;
  std::vector<std::uint64_t> records;
  EXPECT_EQ(ScanJournal(directory_.c_str(), cursor, 3, Collect, &records).code,
            base::ErrorCode::kCorrupt);
  ExpectCursor(cursor, start);
  EXPECT_EQ(records, (std::vector<std::uint64_t>{1}));
}

TEST_F(JournalReaderTest,
       RequiredHeaderDamageFailsProbeWithoutPublishingResult) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  ASSERT_TRUE(journal.Close().ok());
  ChangeByte(Segment(), 16);
  JournalReplayResult result{99, 88, true};
  EXPECT_EQ(ProbeJournal(directory_.c_str(), 1, result).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(result.last_lsn, 99);
  EXPECT_EQ(result.records, 88);
  EXPECT_TRUE(result.tail_discarded);
}

TEST_F(JournalReaderTest, CallbackFailureLeavesCursorAtPreviousCommittedBatch) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  const std::array payload{std::byte{3}};
  std::uint64_t lsn = 0;
  ASSERT_TRUE(journal.TryAppend(7, payload, lsn).ok());
  ASSERT_TRUE(journal.Flush().ok());
  auto cursor = JournalStartCursor(1);
  const auto start = cursor;
  const auto reject = [](void*, std::uint16_t, std::uint64_t,
                         std::span<const std::byte>) noexcept -> base::Status {
    return {base::ErrorCode::kIoError, {}};
  };
  EXPECT_EQ(ScanJournal(directory_.c_str(), cursor, 1, reject, nullptr).code,
            base::ErrorCode::kIoError);
  ExpectCursor(cursor, start);
  EXPECT_TRUE(journal.Close().ok());
}

TEST_F(JournalReaderTest, ReadsStableDurablePrefixesDuringAppendAndRotation) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  ASSERT_TRUE(journal.Start(1).ok());
  std::atomic<bool> finished{false};
  std::vector<std::uint64_t> records;
  auto cursor = JournalStartCursor(1);
  base::Status scan_status;
  std::thread reader([&] {
    for (;;) {
      const bool done = finished.load(std::memory_order_acquire);
      const auto target = journal.durable_lsn();
      if (target > cursor.lsn) {
        scan_status =
            ScanJournal(directory_.c_str(), cursor, target, Collect, &records);
        if (!scan_status.ok()) return;
      }
      if (done && cursor.lsn == journal.durable_lsn()) return;
      std::this_thread::yield();
    }
  });
  std::array<std::byte, 4096> payload{};
  payload.front() = std::byte{3};
  std::uint64_t lsn = 0;
  for (unsigned i = 0; i < 120; ++i) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    base::Status status;
    do {
      status = journal.TryAppend(7, payload, lsn);
      if (status.ok() || !journal.healthy()) break;
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_TRUE(status.ok());
    if (!status.ok()) break;
    if (i % 20 == 19) EXPECT_TRUE(journal.Flush().ok());
  }
  EXPECT_TRUE(journal.Flush().ok());
  finished.store(true, std::memory_order_release);
  reader.join();
  EXPECT_TRUE(scan_status.ok());
  EXPECT_EQ(cursor.lsn, 120);
  ASSERT_EQ(records.size(), 120);
  for (std::size_t i = 0; i < records.size(); ++i) EXPECT_EQ(records[i], i + 1);
  EXPECT_TRUE(journal.Close().ok());
}

TEST_F(JournalReaderTest, ControlFilesCanBeReadUnderWriterLockAndValidateCrc) {
  Journal journal;
  ASSERT_TRUE(journal.Acquire(options_).ok());
  const std::array payload{std::byte{3}, std::byte{5}};
  ASSERT_TRUE(journal.SaveFile("IDENTITY", payload).ok());
  std::array<std::byte, 8> storage{};
  std::size_t size = 99;
  ASSERT_TRUE(
      ReadJournalFile(directory_.c_str(), "IDENTITY", storage, size).ok());
  EXPECT_EQ(size, 2);
  EXPECT_EQ(storage[0], std::byte{3});
  EXPECT_EQ(storage[1], std::byte{5});
  size = 99;
  EXPECT_EQ(ReadJournalFile(directory_.c_str(), "IDENTITY",
                            std::span(storage).first(1), size)
                .code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(size, 99);
  EXPECT_EQ(
      ReadJournalFile(directory_.c_str(), "../IDENTITY", storage, size).code,
      base::ErrorCode::kInvalidArgument);
  ChangeByte(directory_ + "/IDENTITY", 64);
  EXPECT_EQ(ReadJournalFile(directory_.c_str(), "IDENTITY", storage, size).code,
            base::ErrorCode::kCorrupt);
  EXPECT_EQ(size, 99);
  EXPECT_TRUE(journal.Close().ok());
}

}  // namespace
}  // namespace yquant::tradingd
