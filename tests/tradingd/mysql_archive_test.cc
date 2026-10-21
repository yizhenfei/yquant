#include "tradingd/mysql_archive.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>

#include "tradingd/state_codec.h"

namespace yquant::tradingd {
namespace {
using namespace std::chrono_literals;

class ArchiveFilesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-archive-files-XXXXXX";
    ASSERT_NE(mkdtemp(path), nullptr);
    directory_ = path;
    JournalOptions options;
    options.directory = directory_.c_str();
    ASSERT_TRUE(journal_.Acquire(options).ok());
  }
  void TearDown() override {
    EXPECT_TRUE(journal_.Close().ok());
    std::filesystem::remove_all(directory_);
  }
  std::string directory_;
  Journal journal_;

  ServerConfig WorkerConfig() const {
    ServerConfig config;
    std::snprintf(config.persistence.directory,
                  sizeof(config.persistence.directory), "%s",
                  directory_.c_str());
    config.mysql_archive.enabled = true;
    config.mysql_archive.min_free_bytes =
        std::numeric_limits<std::uint64_t>::max();
    config.mysql_archive.retry_ms = 30000;
    config.mysql_archive.shutdown_ms = 50;
    return config;
  }

  static bool WaitForFailureAndDiskSignal(const MysqlArchive& archive) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (archive.failed() && archive.disk_full()) return true;
      std::this_thread::sleep_for(1ms);
    }
    return false;
  }
};
TEST_F(ArchiveFilesTest,
       RunNamesSortAcrossNamespaceGapsAndIgnoreTemporaryFiles) {
  for (const auto run : {1U, 25U, 9U}) {
    char name[96];
    ArchiveName(name, "run", run);
    std::array<std::byte, 64> bytes{};
    StateWriter writer(bytes);
    writer.U64(1);
    writer.U64(run);
    writer.U64(0);
    writer.U64(0);
    writer.U64(1);
    ASSERT_TRUE(
        journal_.SaveFile(name, std::span(bytes).first(writer.size())).ok());
  }
  std::uint64_t id = 0;
  ASSERT_TRUE(NextArchiveRun(directory_.c_str(), 1, id).ok());
  EXPECT_EQ(id, 9);
  ASSERT_TRUE(NextArchiveRun(directory_.c_str(), 9, id).ok());
  EXPECT_EQ(id, 25);
  ASSERT_TRUE(NextArchiveRun(directory_.c_str(), 25, id).ok());
  EXPECT_EQ(id, 0);
  ArchiveRun manifest;
  ASSERT_TRUE(ReadArchiveRun(directory_.c_str(), 9, manifest).ok());
  EXPECT_EQ(manifest.run, 9);
  EXPECT_TRUE(manifest.history_gap);
}
TEST_F(ArchiveFilesTest, RejectsInvalidPredecessorWithoutChangingOutput) {
  char name[96];
  ArchiveName(name, "run", 5);
  std::array<std::byte, 64> bytes{};
  StateWriter writer(bytes);
  for (auto number : {1U, 5U, 6U, 0U, 0U}) writer.U64(number);
  ASSERT_TRUE(
      journal_.SaveFile(name, std::span(bytes).first(writer.size())).ok());
  ArchiveRun manifest{88, 0, 0, false};
  EXPECT_FALSE(ReadArchiveRun(directory_.c_str(), 5, manifest).ok());
  EXPECT_EQ(manifest.run, 88);
}

TEST_F(ArchiveFilesTest, PermanentInputFailureStillPublishesLowDiskAndCancels) {
  // No run manifest is a permanent input failure, before any SQL operation.
  MysqlArchive archive;
  const auto config = WorkerConfig();
  ASSERT_TRUE(archive.Start(config, journal_, 1).ok());
  ASSERT_TRUE(WaitForFailureAndDiskSignal(archive));
  archive.PublishApplied(17);
  EXPECT_EQ(archive.archived_run(), 0);
  EXPECT_EQ(archive.archived_lsn(), 0);
  const auto started = std::chrono::steady_clock::now();
  archive.Stop(false);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}

TEST_F(ArchiveFilesTest, RetryWaitMonitorsDiskAndFinishHonorsItsDeadline) {
  // A published run with a missing baseline is an I/O failure. Keep retry_ms
  // much longer than shutdown_ms to verify that shutdown interrupts the wait.
  char name[96];
  ArchiveName(name, "run", 1);
  std::array<std::byte, 64> bytes{};
  StateWriter writer(bytes);
  for (const auto number : {1U, 1U, 0U, 0U, 0U}) writer.U64(number);
  ASSERT_TRUE(
      journal_.SaveFile(name, std::span(bytes).first(writer.size())).ok());
  MysqlArchive archive;
  const auto config = WorkerConfig();
  ASSERT_TRUE(archive.Start(config, journal_, 1).ok());
  ASSERT_TRUE(WaitForFailureAndDiskSignal(archive));
  archive.PublishApplied(3);
  archive.PublishApplied(19);
  EXPECT_EQ(archive.archived_lsn(), 0);
  const auto started = std::chrono::steady_clock::now();
  archive.Stop(true);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, 20ms);
  EXPECT_LT(elapsed, 1s);
}

TEST_F(ArchiveFilesTest, DisabledWorkerAllowsAppliedAndRepeatedStop) {
  MysqlArchive archive;
  ServerConfig config;
  ASSERT_TRUE(archive.Start(config, journal_, 1).ok());
  archive.PublishApplied(5);
  archive.Stop(true);
  archive.Stop(false);
  EXPECT_FALSE(archive.failed());
  EXPECT_FALSE(archive.disk_full());
  EXPECT_EQ(archive.archived_run(), 0);
  EXPECT_EQ(archive.archived_lsn(), 0);
}
}  // namespace
}  // namespace yquant::tradingd
