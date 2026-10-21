#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "base/internal/logging_backend.h"
#include "gtest/gtest.h"
#include "yquant/base/logging.h"

extern char** environ;

namespace yquant::base {
namespace {

// Owns an exec'd daemon so failure assertions cannot leave a process running.
class LoggingProcessTest : public testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/yquant-logd-XXXXXX";
    char* created = mkdtemp(path);
    ASSERT_NE(created, nullptr);
    directory_ = created;
    queue_ = directory_ + "/queue";
    output_ = directory_ + "/output";
  }

  void TearDown() override {
    if (pid_ > 0) (void)Stop(SIGKILL);
    internal::SetLogBackendHooksForTesting(nullptr);
    for (int index = 1; index <= 100; ++index) {
      (void)unlink((output_ + "." + std::to_string(index)).c_str());
    }
    (void)unlink((queue_ + "2").c_str());
    (void)unlink((output_ + "2").c_str());
    (void)unlink(queue_.c_str());
    (void)unlink(output_.c_str());
    (void)rmdir(directory_.c_str());
  }

  // Returns after logd publishes the fully initialized queue, or after 5s.
  bool Start(std::string capacity = "1048576", std::string limit = "0",
             bool two_channels = false) {
    std::vector<std::string> args{
        YQUANT_LOGD_PATH, "--capacity", capacity, "--max-file-bytes", limit,
        "--channel",      queue_,       output_};
    if (two_channels) {
      args.insert(args.end(), {"--channel", queue_ + "2", output_ + "2"});
    }
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    if (posix_spawn(&pid_, argv[0], nullptr, nullptr, argv.data(), environ) !=
        0) {
      pid_ = -1;
      return false;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (access(queue_.c_str(), F_OK) != 0 ||
           (two_channels && access((queue_ + "2").c_str(), F_OK) != 0)) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  // Waits for the explicitly signaled child and returns its exit status.
  int Stop(int signal = SIGTERM) {
    if (pid_ <= 0) return -1;
    (void)kill(pid_, signal);
    int status = 0;
    pid_t result;
    do {
      result = waitpid(pid_, &status, 0);
    } while (result < 0 && errno == EINTR);
    pid_ = -1;
    return result > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

  LogConfig ProducerConfig(std::uint64_t timeout = 5000000000ULL) {
    LogConfig config;
    config.backend = LogBackend::kLogd;
    config.shutdown_timeout_ns = timeout;
    return config;
  }

  LoggerConfig ChannelConfig(std::uint64_t capacity = 1048576) {
    LoggerConfig config;
    config.name = "process-test";
    config.queue_path = queue_;
    config.queue_capacity = capacity;
    return config;
  }

  std::string Contents(const std::string& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
  }

  std::string directory_;
  std::string queue_;
  std::string output_;
  pid_t pid_ = -1;
};

TEST_F(LoggingProcessTest,
       TransfersOrderedRecordsAndAcknowledgesDurableShutdown) {
  ASSERT_TRUE(Start());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig()).ok());
  const auto created = manager.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  for (int index = 0; index < 1000; ++index) {
    YQ_LOG_INFO(*created.logger, "sequence={}", index);
  }
  const auto result = manager.Shutdown();
  ASSERT_TRUE(result.status.ok());
  EXPECT_EQ(result.counters.enqueued_records_total, 1000U);
  EXPECT_EQ(manager.Shutdown().counters.enqueued_records_total, 1000U);
  EXPECT_TRUE(manager.channel_error(0).status.ok());
  const auto text = Contents(output_);
  std::size_t position = 0;
  for (int index = 0; index < 1000; ++index) {
    const auto found =
        text.find("sequence=" + std::to_string(index) + "\n", position);
    ASSERT_NE(found, std::string::npos);
    position = found + 1;
  }
  EXPECT_EQ(Stop(), 0);
  EXPECT_NE(access(queue_.c_str(), F_OK), 0);
}

TEST_F(LoggingProcessTest, MultipleChannelsHaveIndependentLifetimes) {
  ASSERT_TRUE(Start("1048576", "0", true));
  LogManager first;
  LogManager second;
  ASSERT_TRUE(first.Initialize(ProducerConfig()).ok());
  ASSERT_TRUE(second.Initialize(ProducerConfig()).ok());
  auto first_logger = first.CreateLogger(ChannelConfig());
  auto other = ChannelConfig();
  const auto second_queue = queue_ + "2";
  other.queue_path = second_queue;
  auto second_logger = second.CreateLogger(other);
  ASSERT_TRUE(first_logger.status.ok());
  ASSERT_TRUE(second_logger.status.ok());
  YQ_LOG_INFO(*second_logger.logger, "second-channel");
  ASSERT_TRUE(second.Shutdown().status.ok());
  YQ_LOG_INFO(*first_logger.logger, "first-still-active");
  ASSERT_TRUE(first.Shutdown().status.ok());
  EXPECT_EQ(Stop(), 0);
  EXPECT_NE(Contents(output_).find("first-still-active\n"), std::string::npos);
  EXPECT_EQ(Contents(output_).find("second-channel"), std::string::npos);
  EXPECT_NE(Contents(output_ + "2").find("second-channel\n"),
            std::string::npos);
}

TEST_F(LoggingProcessTest, RejectsMissingQueueAndMismatchedCapacity) {
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig()).ok());
  EXPECT_EQ(manager.CreateLogger(ChannelConfig()).status.system_error.value(),
            ENOENT);
  ASSERT_TRUE(Start());
  EXPECT_EQ(manager.CreateLogger(ChannelConfig(2048)).status.code,
            ErrorCode::kInvalidArgument);
  auto created = manager.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_EQ(Stop(), 0);
}

TEST_F(LoggingProcessTest, RejectsAnotherProducerAndSessionReuse) {
  ASSERT_TRUE(Start());
  LogManager first;
  LogManager second;
  ASSERT_TRUE(first.Initialize(ProducerConfig()).ok());
  ASSERT_TRUE(second.Initialize(ProducerConfig()).ok());
  ASSERT_TRUE(first.CreateLogger(ChannelConfig()).status.ok());
  EXPECT_EQ(second.CreateLogger(ChannelConfig()).status.code,
            ErrorCode::kInvalidState);
  ASSERT_TRUE(first.Shutdown().status.ok());
  EXPECT_EQ(second.CreateLogger(ChannelConfig()).status.code,
            ErrorCode::kInvalidState);
  EXPECT_TRUE(second.Shutdown().status.ok());
  EXPECT_EQ(Stop(), 0);
}

TEST_F(LoggingProcessTest, FullQueueDropsWithoutBlockingAndDrainsOnResume) {
  ASSERT_TRUE(Start("2048"));
  ASSERT_EQ(kill(pid_, SIGSTOP), 0);
  // waitpid confirms the consumer cannot advance while the queue is filled.
  int status = 0;
  ASSERT_EQ(waitpid(pid_, &status, WUNTRACED), pid_);
  ASSERT_TRUE(WIFSTOPPED(status));
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig()).ok());
  auto created = manager.CreateLogger(ChannelConfig(2048));
  ASSERT_TRUE(created.status.ok());
  for (int index = 0; index < 100; ++index) {
    YQ_LOG_INFO(*created.logger, "full={}", index);
  }
  EXPECT_GT(created.logger->counters().queue_dropped_records_total, 0U);
  ASSERT_EQ(kill(pid_, SIGCONT), 0);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_EQ(Stop(), 0);
}

TEST_F(LoggingProcessTest, DeadDaemonTimesOutAndPreservesAcceptedQueue) {
  ASSERT_TRUE(Start());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig(20000000)).ok());
  auto created = manager.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  EXPECT_EQ(Stop(SIGKILL), -1);
  YQ_LOG_INFO(*created.logger, "pending-after-crash");
  auto result = manager.Shutdown();
  EXPECT_EQ(result.status.system_error.value(), ETIMEDOUT);
  EXPECT_EQ(result.stage, LogErrorStage::kSync);
  EXPECT_EQ(result.counters.enqueued_records_total, 1U);
  EXPECT_EQ(manager.channel_error(0).status.system_error.value(), ETIMEDOUT);
  EXPECT_EQ(manager.Shutdown().status.system_error.value(), ETIMEDOUT);
  EXPECT_EQ(access(queue_.c_str(), F_OK), 0);
}

TEST_F(LoggingProcessTest, EarlyConsumerShutdownReportsBrokenPipe) {
  ASSERT_TRUE(Start());
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig()).ok());
  auto created = manager.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  YQ_LOG_INFO(*created.logger, "before-stop");
  EXPECT_EQ(Stop(), 1);
  EXPECT_EQ(created.logger->backend_status().status.system_error.value(),
            EPIPE);
  int evaluations = 0;
  YQ_LOG_INFO(*created.logger, "after-stop={}", ++evaluations);
  EXPECT_EQ(evaluations, 0);
  EXPECT_EQ(manager.Shutdown().status.system_error.value(), EPIPE);
}

TEST_F(LoggingProcessTest, RotationIsPerformedInDaemon) {
  ASSERT_TRUE(Start("1048576", "400"));
  LogManager manager;
  ASSERT_TRUE(manager.Initialize(ProducerConfig()).ok());
  auto created = manager.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  for (int index = 0; index < 30; ++index) {
    YQ_LOG_INFO(*created.logger, "rotate={}", index);
  }
  ASSERT_TRUE(manager.Shutdown().status.ok());
  EXPECT_EQ(Stop(), 0);
  EXPECT_NE(Contents(output_ + ".1").find("rotate=0\n"), std::string::npos);
  EXPECT_NE(Contents(output_).find("rotate=29\n"), std::string::npos);
}

// In-process sinks allow deterministic backend faults, using the same shared
// transport and shutdown handshake as the executable.
TEST_F(LoggingProcessTest, FileErrorIsVisibleToProducerAndBothShutdowns) {
  internal::LogBackendHooks hooks;
  hooks.write = [](void*, int, const void*, std::size_t) noexcept -> ssize_t {
    errno = ENOSPC;
    return -1;
  };
  internal::SetLogBackendHooksForTesting(&hooks);
  LogManager sink;
  ASSERT_TRUE(sink.Initialize().ok());
  auto sink_config = ChannelConfig();
  sink_config.file_path = output_;
  ASSERT_TRUE(sink.CreateLogSink(sink_config).ok());
  LogManager producer;
  ASSERT_TRUE(producer.Initialize(ProducerConfig()).ok());
  auto created = producer.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  YQ_LOG_INFO(*created.logger, "disk-full");
  const auto result = producer.Shutdown();
  EXPECT_EQ(result.status.system_error.value(), ENOSPC);
  EXPECT_EQ(result.stage, LogErrorStage::kWrite);
  EXPECT_EQ(sink.Shutdown().status.system_error.value(), ENOSPC);
}

TEST_F(LoggingProcessTest, SyncFailureCannotAcknowledgeSuccess) {
  internal::LogBackendHooks hooks;
  hooks.sync = [](void*, int) noexcept {
    errno = EIO;
    return -1;
  };
  internal::SetLogBackendHooksForTesting(&hooks);
  LogManager sink;
  ASSERT_TRUE(sink.Initialize().ok());
  auto config = ChannelConfig();
  config.file_path = output_;
  ASSERT_TRUE(sink.CreateLogSink(config).ok());
  LogManager producer;
  ASSERT_TRUE(producer.Initialize(ProducerConfig()).ok());
  auto created = producer.CreateLogger(ChannelConfig());
  ASSERT_TRUE(created.status.ok());
  YQ_LOG_INFO(*created.logger, "sync-failure");
  const auto result = producer.Shutdown();
  EXPECT_EQ(result.status.system_error.value(), EIO);
  EXPECT_EQ(result.stage, LogErrorStage::kSync);
  EXPECT_NE(Contents(output_).find("sync-failure\n"), std::string::npos);
  EXPECT_EQ(sink.Shutdown().status.system_error.value(), EIO);
}

TEST_F(LoggingProcessTest, ExistingQueueIsNeverReplaced) {
  ASSERT_TRUE(Start());
  LogManager duplicate;
  ASSERT_TRUE(duplicate.Initialize().ok());
  auto config = ChannelConfig();
  // Keep the view alive while CreateLogSink copies it.
  const auto other = directory_ + "/other";
  config.file_path = other;
  EXPECT_EQ(duplicate.CreateLogSink(config).system_error.value(), EEXIST);
  EXPECT_TRUE(duplicate.Shutdown().status.ok());
  EXPECT_EQ(access(queue_.c_str(), F_OK), 0);
  EXPECT_EQ(Stop(), 0);
  (void)unlink(other.c_str());
}

}  // namespace
}  // namespace yquant::base
