#include <arpa/inet.h>
#include <fcntl.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "trading_gateway.h"
#include "yquant/md/quote_codec.h"
#include "yquant/md/shm_broadcast.h"

extern char** environ;

namespace yquant::testing {
namespace {

constexpr auto kProcessTimeout = std::chrono::seconds(5);

// Captures each stream independently. pread leaves the child's shared file
// offset unchanged and exposes output flushed before the child terminates.
class ConsoleProcess {
 public:
  ConsoleProcess() = default;
  ConsoleProcess(const ConsoleProcess&) = delete;
  ConsoleProcess& operator=(const ConsoleProcess&) = delete;
  ~ConsoleProcess() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      while (::waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {
      }
    }
    if (stdout_ != nullptr) {
      std::fclose(stdout_);
    }
    if (stderr_ != nullptr) {
      std::fclose(stderr_);
    }
  }

  // Borrows arguments only while spawning. Failed starts leave no live child.
  int Start(const char* executable, const std::vector<std::string>& arguments) {
    auto adapted = arguments;
    if (std::string_view(executable) == YQUANT_TEST_STRATEGYD_PATH) {
      const int error = gateway_.Prepare(adapted);
      if (error) {
        return error;
      }
    }
    stdout_ = std::tmpfile();
    stderr_ = std::tmpfile();
    if (stdout_ == nullptr || stderr_ == nullptr) {
      return errno;
    }
    const int output_fd = ::fileno(stdout_);
    const int error_fd = ::fileno(stderr_);
    if (::fcntl(output_fd, F_SETFD, FD_CLOEXEC) < 0 ||
        ::fcntl(error_fd, F_SETFD, FD_CLOEXEC) < 0) {
      return errno;
    }
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), adapted.begin(), adapted.end());
    std::vector<char*> argv;
    for (auto& argument : storage) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = ::posix_spawn_file_actions_init(&actions);
    if (error != 0) {
      return error;
    }
    error =
        ::posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
    if (error == 0) {
      error =
          ::posix_spawn_file_actions_adddup2(&actions, error_fd, STDERR_FILENO);
    }
    if (error == 0) {
      error = ::posix_spawn(&pid_, executable, &actions, nullptr, argv.data(),
                            environ);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    return error;
  }

  // Requires the readiness message to be visible while the child is alive;
  // output buffered until exit does not satisfy this check.
  bool WaitForRunning() {
    const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const bool ready = stdout_text().find(": Running ") != std::string::npos;
      if (!IsRunning()) {
        return false;
      }
      if (ready) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  bool Signal(int signal) { return pid_ > 0 && ::kill(pid_, signal) == 0; }

  bool Wait() {
    const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!IsRunning()) {
        return reaped_;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }

  int exit_code() const {
    return reaped_ && WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }
  std::string stdout_text() const { return Read(stdout_); }
  std::string stderr_text() const { return Read(stderr_); }

 private:
  bool IsRunning() {
    if (pid_ <= 0) {
      return false;
    }
    pid_t result;
    do {
      result = ::waitpid(pid_, &status_, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      return true;
    }
    reaped_ = result == pid_;
    pid_ = -1;
    return false;
  }

  // Reads an independent snapshot, preserving the writer's append offset.
  static std::string Read(FILE* file) {
    std::string text;
    if (file == nullptr) {
      return text;
    }
    char buffer[1024];
    for (;;) {
      const auto count =
          ::pread(::fileno(file), buffer, sizeof(buffer), text.size());
      if (count > 0) {
        text.append(buffer, static_cast<size_t>(count));
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        return text;
      }
    }
  }

  FILE* stdout_ = nullptr;
  FILE* stderr_ = nullptr;
  TradingGateway gateway_;
  pid_t pid_ = -1;
  int status_ = 0;
  bool reaped_ = false;
};

struct Program {
  const char* executable;
  bool strategy;
};

class ConsoleUsageTest : public ::testing::TestWithParam<Program> {};

TEST_P(ConsoleUsageTest, HelpGoesToStdout) {
  ConsoleProcess child;
  ASSERT_EQ(child.Start(GetParam().executable, {"--help"}), 0);
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0);
  EXPECT_NE(child.stdout_text().find("Usage:"), std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_P(ConsoleUsageTest, InvalidArgumentsGoToStderr) {
  ConsoleProcess child;
  ASSERT_EQ(child.Start(GetParam().executable, {"--unknown"}), 0);
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 2);
  EXPECT_TRUE(child.stdout_text().empty()) << child.stdout_text();
  EXPECT_NE(child.stderr_text().find("Usage:"), std::string::npos);
}

TEST_P(ConsoleUsageTest, ConfigurationFailureGoesToStderr) {
  const char* missing = "/yquant-missing-console-test/config.json";
  std::vector<std::string> arguments{"--config", missing};
  if (GetParam().strategy) {
    arguments.insert(arguments.end(), {"--strategy", YQUANT_TEST_STRATEGY_PATH,
                                       "--market-data-config", missing,
                                       "--trading-config", missing});
  }
  ConsoleProcess child;
  ASSERT_EQ(child.Start(GetParam().executable, arguments), 0);
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 1);
  EXPECT_TRUE(child.stdout_text().empty()) << child.stdout_text();
  EXPECT_NE(child.stderr_text().find("stage="), std::string::npos);
}

INSTANTIATE_TEST_SUITE_P(
    Programs, ConsoleUsageTest,
    ::testing::Values(Program{YQUANT_TEST_STRATEGYD_PATH, true},
                      Program{YQUANT_TEST_MARKETD_PATH, false},
                      Program{YQUANT_TEST_TRADINGD_PATH, false}));

// Configuration lives in a private runtime directory, with absolute references
// so these process tests do not change the parent's working directory.
class ConsoleOutputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NE(::mkdtemp(directory_), nullptr);
    owns_directory_ = true;
    const std::string catalog = YQUANT_TEST_INSTRUMENT_CATALOG_PATH;
    ASSERT_TRUE(
        Write("market.json",
              R"({"adapter":"shm","source_id":1,"instrument_catalog":")" +
                  catalog + R"(","runtime_directory":")" + directory_ +
                  R"(","stream":"quotes","poll_interval_us":1000,)"
                  R"("instruments":[{"venue":"SSE","symbol":"600000"}]})"));
    ASSERT_TRUE(Write(
        "mock.json", R"({"adapter":"mock","source_id":1,"interval_ms":1,)"
                     R"("instrument_catalog":")" +
                         catalog +
                         R"(","instruments":[{"venue":"SSE","symbol":"600000",)"
                         R"("bid_price":1000,"bid_qty":100,"ask_price":1001,)"
                         R"("ask_qty":200,"price_step":0}]})"));
    ASSERT_TRUE(
        Write("marketd.json",
              R"({"source":{"adapter":"mock","config":")" + File("mock.json") +
                  R"("},"publishers":[{"adapter":"shm","runtime_directory":")" +
                  directory_ + R"(","stream":"published"}]})"));
  }

  void TearDown() override {
    if (owns_directory_) {
      std::error_code error;
      ::yquant::testing::files::RemoveTree(
          ::yquant::testing::files::FilePath(directory_), error);
      EXPECT_FALSE(error) << error.message();
    }
  }

  std::string File(std::string_view name) const {
    return std::string(directory_) + "/" + std::string(name);
  }

  bool Write(std::string_view name, std::string_view contents) const {
    auto* file = std::fopen(File(name).c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const bool written = std::fwrite(contents.data(), 1, contents.size(),
                                     file) == contents.size();
    return std::fclose(file) == 0 && written;
  }

  std::vector<std::string> StrategyArguments() const {
    return {"--strategy",
            YQUANT_TEST_STRATEGY_PATH,
            "--config",
            YQUANT_TEST_STRATEGY_CONFIG_PATH,
            "--market-data-config",
            File("market.json"),
            "--trading-config",
            YQUANT_TEST_BROKER_CONFIG_PATH};
  }

  // JSON requires a nonzero port; reserve a candidate using an ephemeral bind.
  void PrepareTradingConfig() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const bool bound =
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    EXPECT_EQ(::close(fd), 0);
    ASSERT_TRUE(bound);
    const auto port = ntohs(address.sin_port);
    ASSERT_NE(port, 0);
    ASSERT_TRUE(Write("broker.json",
                      R"({"adapter":"mock","mode":"fill",)"
                      R"("report_interval_ms":1,"shutdown_timeout_ms":100, )"
                      R"("initial_cash":100000000,"initial_positions":[]})"));
    ASSERT_TRUE(Write("tradingd.json",
                      R"({"listen":{"address":"127.0.0.1","port":)" +
                          std::to_string(port) + R"(},"market_data_config":")" +
                          File("market.json") + R"(","trading_config":")" +
                          File("broker.json") +
                          R"(","token":"console-test",)"
                          R"("shutdown_timeout_ms":1000})"));
  }

  char directory_[64] = "/tmp/yquant-console-XXXXXX";
  bool owns_directory_ = false;
};

TEST_F(ConsoleOutputTest, MarketdConfigurationSuccessGoesToStdout) {
  ConsoleProcess child;
  ASSERT_EQ(child.Start(YQUANT_TEST_MARKETD_PATH,
                        {"--config", File("marketd.json"), "--check-config"}),
            0);
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0) << child.stderr_text();
  EXPECT_NE(child.stdout_text().find("configuration valid"), std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_F(ConsoleOutputTest, TradingdConfigurationSuccessGoesToStdout) {
  PrepareTradingConfig();
  ASSERT_FALSE(HasFatalFailure());
  ConsoleProcess child;
  ASSERT_EQ(child.Start(YQUANT_TEST_TRADINGD_PATH,
                        {"--config", File("tradingd.json"), "--check-config"}),
            0);
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0) << child.stderr_text();
  EXPECT_NE(child.stdout_text().find("configuration valid"), std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_F(ConsoleOutputTest, MarketdLifecycleIsFlushedToStdout) {
  ConsoleProcess child;
  ASSERT_EQ(
      child.Start(YQUANT_TEST_MARKETD_PATH, {"--config", File("marketd.json")}),
      0);
  ASSERT_TRUE(child.WaitForRunning()) << child.stderr_text();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0) << child.stderr_text();
  EXPECT_NE(child.stdout_text().find("Stopped events="), std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_F(ConsoleOutputTest, TradingdLifecycleIsFlushedToStdout) {
  PrepareTradingConfig();
  ASSERT_FALSE(HasFatalFailure());
  ConsoleProcess child;
  ASSERT_EQ(child.Start(YQUANT_TEST_TRADINGD_PATH,
                        {"--config", File("tradingd.json")}),
            0);
  ASSERT_TRUE(child.WaitForRunning()) << child.stderr_text();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0) << child.stderr_text();
  EXPECT_NE(child.stdout_text().find("Stopped\n"), std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_F(ConsoleOutputTest, StrategydLifecycleIsFlushedToStdout) {
  md::ShmPublisher publisher;
  ASSERT_TRUE(
      publisher.Create({{directory_, "quotes"}, 16, md::kQuoteFrameSize}).ok());
  ConsoleProcess child;
  ASSERT_EQ(child.Start(YQUANT_TEST_STRATEGYD_PATH, StrategyArguments()), 0);
  ASSERT_TRUE(child.WaitForRunning()) << child.stderr_text();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 0) << child.stderr_text();
  EXPECT_NE(child.stdout_text().find("lifecycle_strategy: startup_message="),
            std::string::npos);
  EXPECT_TRUE(child.stderr_text().empty()) << child.stderr_text();
}

TEST_F(ConsoleOutputTest, StrategydSeparatesSummaryFromRuntimeFailure) {
  md::ShmPublisher publisher;
  ASSERT_TRUE(
      publisher.Create({{directory_, "quotes"}, 16, md::kQuoteFrameSize}).ok());
  ConsoleProcess child;
  ASSERT_EQ(child.Start(YQUANT_TEST_STRATEGYD_PATH,
                        {"--strategy", YQUANT_TEST_QUOTE_STRATEGY_PATH,
                         "--config", YQUANT_TEST_QUOTE_STRATEGY_CONFIG_PATH,
                         "--market-data-config", File("market.json"),
                         "--trading-config", YQUANT_TEST_BROKER_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForRunning()) << child.stderr_text();
  model::QuoteTick quote;
  quote.source_id = {1};
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", quote.instrument_id),
            base::ErrorCode::kOk);
  quote.sequence = 1;
  md::QuoteFrame frame;
  ASSERT_TRUE(md::EncodeQuote(quote, frame).ok());
  ASSERT_TRUE(publisher.Publish(md::QuoteEvent(quote, frame)).ok());
  ASSERT_TRUE(publisher.Publish(md::QuoteEvent(quote, frame)).ok());
  // A corrupt frame after the duplicate makes consumption observable without
  // timing assumptions: the reader must process both earlier records first.
  frame[0] = std::byte{0};
  ASSERT_TRUE(publisher.Publish(md::QuoteEvent(quote, frame)).ok());
  ASSERT_TRUE(child.Wait());
  EXPECT_EQ(child.exit_code(), 1);
  EXPECT_NE(child.stdout_text().find("quote_observer: ticks=1 last_sequence=1"),
            std::string::npos);
  EXPECT_NE(child.stdout_text().find("ignored_quotes=1\n"), std::string::npos);
  EXPECT_EQ(child.stdout_text().find("stage="), std::string::npos);
  EXPECT_NE(child.stderr_text().find("stage=market_data_poll"),
            std::string::npos);
  EXPECT_EQ(child.stderr_text().find("ignored_quotes="), std::string::npos);
  EXPECT_EQ(child.stderr_text().find(": Running "), std::string::npos);
}

}  // namespace
}  // namespace yquant::testing
