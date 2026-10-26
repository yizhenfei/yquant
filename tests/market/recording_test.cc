#include "yquant/market/recording.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
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
#include "market/pipeline_test_support.h"
#include "yquant/base/format.h"

namespace yquant::market {
namespace {

RecordConfig Config(const std::string& path, size_t capacity = 1 << 20) {
  RecordConfig config;
  ::yquant::base::FormatTo(config.path, sizeof(config.path), "{}",
                           path.c_str());
  config.queue_capacity = capacity;
  return config;
}

std::vector<std::byte> ReadBytes(const std::string& path) {
  FILE* file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return {};
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return {};
  }
  const auto size = std::ftell(file);
  if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return {};
  }
  std::vector<std::byte> bytes(static_cast<size_t>(size));
  const bool complete =
      std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
  std::fclose(file);
  if (!complete) {
    bytes.clear();
  }
  return bytes;
}

bool WriteBytes(const std::string& path, std::span<const std::byte> bytes) {
  FILE* file = std::fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const bool complete =
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  return std::fclose(file) == 0 && complete;
}

void Store64(std::span<std::byte> bytes, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::byte>(value & 0xff);
    value >>= 8;
  }
}

void RepairChecksum(std::span<std::byte> bytes) {
  uint64_t value = 14695981039346656037ULL;
  for (const auto byte : bytes.first(bytes.size() - 8)) {
    value ^= std::to_integer<uint8_t>(byte);
    value *= 1099511628211ULL;
  }
  Store64(bytes.last(8), value);
}

bool CreateRecording(const std::string& path, size_t count = 3) {
  RecordPublisher publisher;
  if (!publisher.Init(Config(path)).ok()) {
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    auto quote = testing::Quote(i + 1);
    quote.receive_time_ns = 1000 + i * 100;
    if (!publisher.Publish(quote).ok()) {
      return false;
    }
  }
  return publisher.Finish().ok();
}

TEST(RecordingTest, FinishDrainsAllFramesAndPreservesQuoteMetadata) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("quotes.yqr");
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(path)).ok());
  for (size_t i = 0; i < 1000; ++i) {
    auto quote = testing::Quote(i + 1);
    quote.receive_time_ns = 1000 + i * 100;
    ASSERT_TRUE(publisher.Publish(quote).ok()) << publisher.error_detail();
  }
  ASSERT_TRUE(publisher.Finish().ok()) << publisher.error_detail();
  EXPECT_TRUE(publisher.Finish().ok());
  publisher.Close();
  EXPECT_EQ(publisher.Publish(testing::Quote()).code,
            base::ErrorCode::kInvalidState);
  ReplayData data;
  std::array<char, 256> detail;
  ASSERT_TRUE(LoadRecordingFile(path.c_str(), 1000, data, detail).ok())
      << detail.data();
  ASSERT_EQ(data.size(), 1000U);
  for (size_t i = 0; i < data.size(); ++i) {
    auto expected = testing::Quote(i + 1);
    expected.receive_time_ns = 0;
    EXPECT_EQ(data[i].quote, expected);
    EXPECT_EQ(data[i].offset_ns, i * 100);
  }
  EXPECT_EQ(ReadBytes(path).size(),
            kRecordingHeaderSize + 1000 * kRecordingRecordSize);
}

TEST(RecordingTest, EmptyRecordingIsSealedAndLoadable) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("empty.yqr");
  ASSERT_TRUE(CreateRecording(path, 0));
  ReplayData data;
  ASSERT_TRUE(LoadRecordingFile(path.c_str(), 1, data, {}).ok());
  EXPECT_EQ(data.size(), 0U);
}

TEST(RecordingTest, ReaderRejectsFifoWithoutWaitingForAWriter) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("quotes.fifo");
  ASSERT_EQ(::mkfifo(path.c_str(), 0600), 0);
  ReplayData data;
  EXPECT_EQ(LoadRecordingFile(path.c_str(), 10, data, {}).code,
            base::ErrorCode::kInvalidArgument);
}

TEST(RecordingTest, FinishUsesTheOpenedParentDirectoryAfterRename) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto original = directory.File("original");
  const auto moved = directory.File("moved");
  std::error_code error;
  ASSERT_TRUE(::yquant::testing::files::CreateDirectory(
      ::yquant::testing::files::FilePath(original), error));
  ASSERT_FALSE(error);
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(original + "/quotes.yqr")).ok());
  ::yquant::testing::files::Rename(::yquant::testing::files::FilePath(original),
                                   ::yquant::testing::files::FilePath(moved),
                                   error);
  ASSERT_FALSE(error);
  ASSERT_TRUE(publisher.Publish(testing::Quote()).ok());
  ASSERT_TRUE(publisher.Finish().ok()) << publisher.error_detail();
  ReplayData data;
  ASSERT_TRUE(
      LoadRecordingFile((moved + "/quotes.yqr").c_str(), 1, data, {}).ok());
  EXPECT_EQ(data.size(), 1U);
}

TEST(RecordingTest, ExistingOutputIsNeverOverwritten) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("exists.yqr", "preserve me"));
  const auto path = directory.File("exists.yqr");
  RecordPublisher publisher;
  const auto status = publisher.Init(Config(path));
  EXPECT_EQ(status.code, base::ErrorCode::kIoError);
  EXPECT_EQ(status.system_error.value(), EEXIST);
  const auto bytes = ReadBytes(path);
  ASSERT_EQ(bytes.size(), 11U);
  EXPECT_EQ(std::memcmp(bytes.data(), "preserve me", bytes.size()), 0);
}

TEST(RecordingTest, RejectsInvalidQueueAndMissingParentBeforeCreatingFile) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("invalid.yqr");
  for (const auto capacity : {0U, 128U, 257U, 128U * 1024 * 1024}) {
    RecordPublisher publisher;
    EXPECT_EQ(publisher.Init(Config(path, capacity)).code,
              base::ErrorCode::kInvalidArgument);
    EXPECT_NE(::access(path.c_str(), F_OK), 0);
  }
  RecordPublisher publisher;
  EXPECT_EQ(publisher.Init(Config(directory.File("missing/file.yqr"))).code,
            base::ErrorCode::kIoError);
}

TEST(RecordingTest, CorruptFilesPreservePreviousReplayData) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto good_path = directory.File("good.yqr");
  const auto bad_path = directory.File("bad.yqr");
  ASSERT_TRUE(CreateRecording(good_path));
  const auto original = ReadBytes(good_path);
  ASSERT_EQ(original.size(), 64U + 3U * 96U);
  ReplayData output;
  ASSERT_TRUE(output.Reserve(1).ok());
  const ReplayRecord sentinel{999, testing::Quote(999)};
  ASSERT_TRUE(output.Append(sentinel).ok());
  for (int mutation = 0; mutation < 13; ++mutation) {
    SCOPED_TRACE(mutation);
    auto bytes = original;
    auto header = std::span(bytes).first(64);
    if (mutation == 0) {
      bytes.resize(10);
    }
    if (mutation == 1) {
      bytes.pop_back();
    }
    if (mutation == 2) {
      bytes.push_back(std::byte{0});
    }
    if (mutation == 3) {
      bytes[24] = std::byte{0};  // Unsealed, with a valid header checksum.
      RepairChecksum(header);
    }
    if (mutation == 4) {
      bytes[0] ^= std::byte{1};
    }
    if (mutation == 5) {
      bytes[8] = std::byte{2};
      RepairChecksum(header);
    }
    if (mutation == 6) {
      bytes[28] = std::byte{1};
      RepairChecksum(header);
    }
    if (mutation == 7) {
      bytes[56] ^= std::byte{1};
    }
    if (mutation == 8) {
      bytes[64 + 8 + 40] ^= std::byte{1};
    }
    if (mutation == 9) {
      bytes[64 + 8 + 8] = std::byte{0};  // Invalid zero source_id.
      RepairChecksum(std::span(bytes).subspan(64, 96));
    }
    if (mutation == 10) {
      Store64(std::span(bytes).subspan(64, 8), 1);  // First offset must be 0.
      RepairChecksum(std::span(bytes).subspan(64, 96));
    }
    if (mutation == 11) {
      Store64(std::span(bytes).subspan(64 + 2 * 96, 8), 99);
      RepairChecksum(std::span(bytes).subspan(64 + 2 * 96, 96));
    }
    if (mutation == 12) {
      Store64(header.subspan(40, 8), 201);
      RepairChecksum(header);
    }
    ASSERT_TRUE(WriteBytes(bad_path, bytes));
    std::array<char, 80> detail;
    EXPECT_EQ(LoadRecordingFile(bad_path.c_str(), 10, output, detail).code,
              base::ErrorCode::kCorrupt);
    EXPECT_NE(detail.front(), '\0');
    ASSERT_EQ(output.size(), 1U);
    EXPECT_EQ(output[0].offset_ns, sentinel.offset_ns);
    EXPECT_EQ(output[0].quote, sentinel.quote);
  }
}

TEST(RecordingTest, RecordLimitRejectsFileWithoutChangingOutput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("quotes.yqr");
  ASSERT_TRUE(CreateRecording(path));
  ReplayData data;
  ASSERT_TRUE(data.Reserve(1).ok());
  ASSERT_TRUE(data.Append({77, testing::Quote(77)}).ok());
  EXPECT_EQ(LoadRecordingFile(path.c_str(), 2, data, {}).code,
            base::ErrorCode::kResourceExhausted);
  ASSERT_EQ(data.size(), 1U);
  EXPECT_EQ(data[0].offset_ns, 77U);
}

TEST(RecordingTest, BackwardReceiveTimeIsTerminalAndLeavesFileUnsealed) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("backward.yqr");
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(path)).ok());
  auto quote = testing::Quote();
  ASSERT_TRUE(publisher.Publish(quote).ok());
  --quote.receive_time_ns;
  EXPECT_EQ(publisher.Publish(quote).code, base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(publisher.Finish().code, base::ErrorCode::kInvalidArgument);
  ReplayData data;
  EXPECT_EQ(LoadRecordingFile(path.c_str(), 10, data, {}).code,
            base::ErrorCode::kCorrupt);
}

struct IoControl {
  std::atomic<bool> blocked{false};
  std::atomic<bool> entered{false};
  bool fail_write = false;
  int fail_sync_call = 0;
  int sync_calls = 0;
  bool fail_close = false;
  bool interrupt_once = false;
  size_t short_write = 0;

  static ssize_t Write(void* context, int fd, const void* data,
                       size_t size) noexcept {
    auto& control = *static_cast<IoControl*>(context);
    control.entered.store(true, std::memory_order_release);
    while (control.blocked.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    if (control.interrupt_once) {
      control.interrupt_once = false;
      errno = EINTR;
      return -1;
    }
    if (control.fail_write) {
      errno = ENOSPC;
      return -1;
    }
    if (control.short_write != 0) {
      size = std::min(size, control.short_write);
    }
    return ::write(fd, data, size);
  }

  static int Sync(void* context, int fd) noexcept {
    auto& control = *static_cast<IoControl*>(context);
    if (++control.sync_calls == control.fail_sync_call) {
      errno = EIO;
      return -1;
    }
    return ::fsync(fd);
  }

  static int Close(void* context, int fd) noexcept {
    const int result = ::close(fd);
    if (static_cast<IoControl*>(context)->fail_close) {
      errno = EIO;
      return -1;
    }
    return result;
  }

  RecordIoHooks hooks() noexcept { return {this, Write, Sync, Close}; }

  bool WaitForWrite() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!entered.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }
};

TEST(RecordingTest, QueueFullIsReportedAndRecordingRemainsUnsealed) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("full.yqr");
  IoControl control;
  control.blocked.store(true);
  const auto hooks = control.hooks();
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(path, 256), &hooks).ok());
  ASSERT_TRUE(publisher.Publish(testing::Quote()).ok());
  const bool entered = control.WaitForWrite();
  if (!entered) {
    control.blocked.store(false);
  }
  ASSERT_TRUE(entered);
  const auto second = publisher.Publish(testing::Quote(2));
  const auto third = publisher.Publish(testing::Quote(3));
  const auto fourth = publisher.Publish(testing::Quote(4));
  control.blocked.store(false, std::memory_order_release);
  EXPECT_TRUE(second.ok());
  EXPECT_TRUE(third.ok());
  EXPECT_EQ(fourth.code, base::ErrorCode::kResourceExhausted);
  EXPECT_NE(std::strstr(publisher.error_detail(), "full"), nullptr);
  EXPECT_EQ(publisher.Finish().code, base::ErrorCode::kResourceExhausted);
  ReplayData data;
  EXPECT_EQ(LoadRecordingFile(path.c_str(), 10, data, {}).code,
            base::ErrorCode::kCorrupt);
}

TEST(RecordingTest, BackendWriteFailureIsReportedAtFinish) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("io.yqr");
  IoControl control;
  control.fail_write = true;
  const auto hooks = control.hooks();
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(path), &hooks).ok());
  ASSERT_TRUE(publisher.Publish(testing::Quote()).ok());
  const auto status = publisher.Finish();
  EXPECT_EQ(status.code, base::ErrorCode::kIoError);
  EXPECT_EQ(status.system_error.value(), ENOSPC);
  EXPECT_EQ(publisher.Finish().system_error.value(), ENOSPC);
  ReplayData data;
  EXPECT_EQ(LoadRecordingFile(path.c_str(), 10, data, {}).code,
            base::ErrorCode::kCorrupt);
}

TEST(RecordingTest, PartialAndInterruptedWritesAreRetried) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = directory.File("partial.yqr");
  IoControl control;
  control.short_write = 7;
  control.interrupt_once = true;
  const auto hooks = control.hooks();
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(Config(path), &hooks).ok());
  ASSERT_TRUE(publisher.Publish(testing::Quote()).ok());
  ASSERT_TRUE(publisher.Finish().ok()) << publisher.error_detail();
  ReplayData data;
  ASSERT_TRUE(LoadRecordingFile(path.c_str(), 10, data, {}).ok());
  EXPECT_EQ(data.size(), 1U);
}

TEST(RecordingTest, FinalSyncAndCloseErrorsPropagate) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  for (int failing_stage : {1, 2, 3}) {
    SCOPED_TRACE(failing_stage);
    IoControl control;
    control.fail_sync_call = failing_stage < 3 ? failing_stage : 0;
    control.fail_close = failing_stage == 3;
    const auto hooks = control.hooks();
    RecordPublisher publisher;
    const auto path = directory.File(std::to_string(failing_stage) + ".yqr");
    ASSERT_TRUE(publisher.Init(Config(path), &hooks).ok());
    ASSERT_TRUE(publisher.Publish(testing::Quote()).ok());
    const auto status = publisher.Finish();
    EXPECT_EQ(status.code, base::ErrorCode::kIoError);
    EXPECT_EQ(status.system_error.value(), EIO);
    EXPECT_EQ(publisher.Finish().code, base::ErrorCode::kIoError);
    ReplayData data;
    if (failing_stage == 1) {
      EXPECT_EQ(LoadRecordingFile(path.c_str(), 10, data, {}).code,
                base::ErrorCode::kCorrupt);
    } else {
      // The complete layout may already be sealed when durability or close
      // reporting fails. Finish's error remains authoritative to its caller.
      EXPECT_TRUE(LoadRecordingFile(path.c_str(), 10, data, {}).ok());
      EXPECT_EQ(data.size(), 1U);
    }
  }
}

}  // namespace
}  // namespace yquant::market
