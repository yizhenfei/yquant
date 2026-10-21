#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "gtest/gtest.h"
#include "market_data_feed.h"
#include "test_config_file.h"

extern char** environ;

namespace yquant::strategyd {
namespace {

constexpr auto kProcessTimeout = std::chrono::seconds(10);

enum class InheritedSignals { kUnchanged, kBlocked, kIgnored };

void CloseIfOpen(int fd) {
  if (fd >= 0) {
    close(fd);
  }
}

int CreateReleaseChannel(int* fds) {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
    return -1;
  }
#ifdef SO_NOSIGPIPE
  const int enabled = 1;
  if (setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) !=
      0) {
    return -1;
  }
#endif
  return 0;
}

class TemporaryDirectory {
 public:
  TemporaryDirectory() { valid_ = mkdtemp(path_) != nullptr; }
  ~TemporaryDirectory() {
    if (valid_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }

  bool valid() const { return valid_; }
  const char* path() const { return path_; }

 private:
  char path_[64] = "/tmp/yquant-strategyd-XXXXXX";
  bool valid_ = false;
};

class ChildProcess {
 public:
  ChildProcess() = default;
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  ~ChildProcess() {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status_, 0) == -1 && errno == EINTR) {
      }
    }
    CloseIfOpen(output_fd_);
    CloseIfOpen(event_fd_);
    CloseIfOpen(release_fd_);
  }

  int Start(const std::vector<std::string>& arguments, bool block_start = false,
            InheritedSignals signals = InheritedSignals::kUnchanged) {
    auto adapted_arguments = arguments;
    const int feed_error = feed_.Prepare(adapted_arguments);
    if (feed_error) return feed_error;
    int output[2] = {-1, -1};
    int events[2] = {-1, -1};
    int release[2] = {-1, -1};
    if (pipe(output) != 0 || pipe(events) != 0 ||
        CreateReleaseChannel(release) != 0) {
      const int error = errno;
      CloseIfOpen(output[0]);
      CloseIfOpen(output[1]);
      CloseIfOpen(events[0]);
      CloseIfOpen(events[1]);
      CloseIfOpen(release[0]);
      CloseIfOpen(release[1]);
      return error;
    }

    std::vector<std::string> argv_storage{YQUANT_TEST_STRATEGYD_PATH};
    argv_storage.insert(argv_storage.end(), adapted_arguments.begin(),
                        adapted_arguments.end());
    if (signals != InheritedSignals::kUnchanged) {
      argv_storage.insert(
          argv_storage.begin(),
          signals == InheritedSignals::kBlocked ? "blocked" : "ignored");
      argv_storage.insert(argv_storage.begin(),
                          YQUANT_TEST_SIGNAL_LAUNCHER_PATH);
    }
    std::vector<char*> argv;
    for (std::string& argument : argv_storage) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_storage;
    for (char** entry = environ; *entry != nullptr; ++entry) {
      if (std::strncmp(*entry, "YQUANT_TEST_EVENT_FD=", 21) != 0 &&
          std::strncmp(*entry, "YQUANT_TEST_START_RELEASE_FD=", 29) != 0) {
        env_storage.emplace_back(*entry);
      }
    }
    env_storage.push_back("YQUANT_TEST_EVENT_FD=" + std::to_string(events[1]));
    if (block_start) {
      env_storage.push_back("YQUANT_TEST_START_RELEASE_FD=" +
                            std::to_string(release[0]));
    }
    std::vector<char*> env;
    for (std::string& entry : env_storage) {
      env.push_back(entry.data());
    }
    env.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0) {
      error =
          posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
      if (error == 0) {
        error = posix_spawn_file_actions_adddup2(&actions, output[1],
                                                 STDERR_FILENO);
      }
      for (int fd : {output[0], output[1], events[0], release[1]}) {
        if (error == 0) {
          error = posix_spawn_file_actions_addclose(&actions, fd);
        }
      }
      if (error == 0) {
        error = posix_spawn(&pid_, argv.front(), &actions, nullptr, argv.data(),
                            env.data());
      }
      posix_spawn_file_actions_destroy(&actions);
    }
    CloseIfOpen(output[1]);
    CloseIfOpen(events[1]);
    CloseIfOpen(release[0]);
    output_fd_ = output[0];
    event_fd_ = events[0];
    release_fd_ = release[1];
    if (error != 0) {
      pid_ = -1;
      return error;
    }
    if (fcntl(output_fd_, F_SETFL, O_NONBLOCK) == -1 ||
        fcntl(event_fd_, F_SETFL, O_NONBLOCK) == -1) {
      return errno;
    }
    return 0;
  }

  bool WaitForOutput(const char* text) {
    return ObserveUntil(
        [&] { return output_.find(text) != std::string::npos; });
  }

  bool WaitForEvent(char event) {
    return ObserveUntil(
        [&] { return events_.find(event) != std::string::npos; });
  }

  bool WaitForEventCount(char event, std::size_t count) {
    return ObserveUntil([&] {
      return static_cast<std::size_t>(
                 std::count(events_.begin(), events_.end(), event)) >= count;
    });
  }

  bool Wait() {
    return ObserveUntil([&] { return pid_ < 0; });
  }

  bool Signal(int signal) { return pid_ > 0 && kill(pid_, signal) == 0; }

  bool ReleaseStart() {
    const char release = 'r';
    ssize_t count;
    do {
#ifdef MSG_NOSIGNAL
      count = send(release_fd_, &release, 1, MSG_NOSIGNAL);
#else
      count = send(release_fd_, &release, 1, 0);
#endif
    } while (count == -1 && errno == EINTR);
    return count == 1;
  }

  int exit_code() const {
    return exited_ && WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }

  const std::string& output() const { return output_; }
  const std::string& events() const { return events_; }

 private:
  static void Drain(int fd, std::string& destination) {
    char buffer[512];
    for (;;) {
      const ssize_t count = read(fd, buffer, sizeof(buffer));
      if (count > 0) {
        destination.append(buffer, static_cast<size_t>(count));
      } else if (count == -1 && errno == EINTR) {
        continue;
      } else {
        return;
      }
    }
  }

  template <typename Predicate>
  bool ObserveUntil(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
    for (;;) {
      Drain(output_fd_, output_);
      Drain(event_fd_, events_);
      if (pid_ > 0) {
        const pid_t result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          exited_ = true;
          Drain(output_fd_, output_);
          Drain(event_fd_, events_);
        } else if (result == -1 && errno != EINTR) {
          return false;
        }
      }
      if (output_.find("Running") != std::string::npos) feed_.Release();
      if (done()) {
        return true;
      }
      if (pid_ < 0) {
        return false;
      }
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) {
        return false;
      }
      pollfd fds[] = {{output_fd_, POLLIN, 0}, {event_fd_, POLLIN, 0}};
      const int result = poll(fds, 2, static_cast<int>(remaining.count()));
      if (result == -1 && errno != EINTR) {
        return false;
      }
    }
  }

  ::yquant::testing::MarketDataFeed feed_;
  pid_t pid_ = -1;
  int output_fd_ = -1;
  int event_fd_ = -1;
  int release_fd_ = -1;
  int status_ = 0;
  bool exited_ = false;
  std::string output_;
  std::string events_;
};

void ExpectCompletedLifecycle(const std::string& events,
                              const std::string& expected) {
  const size_t unload = events.find('U');
  EXPECT_EQ(events.substr(0, unload), expected);
  if (unload != std::string::npos && expected.find('D') != std::string::npos) {
    EXPECT_LT(events.find('D'), unload);
  }
}

// Quote fixtures use one subscription, so Q must occur only between S and T.
void ExpectQuoteLifecycle(const std::string& events, std::size_t min_quotes) {
  const std::string trace = events.substr(0, events.find('U'));
  ASSERT_GE(trace.size(), 5U + min_quotes) << trace;
  EXPECT_EQ(trace.substr(0, 3), "CPS");
  EXPECT_EQ(trace.substr(trace.size() - 2), "TD");
  const std::string quotes = trace.substr(3, trace.size() - 5);
  EXPECT_GE(quotes.size(), min_quotes);
  EXPECT_EQ(quotes, std::string(quotes.size(), 'Q'));
}

std::string HostMarketConfig(
    std::uint32_t interval_ms, std::uint32_t source_id = 1,
    std::string_view catalog = YQUANT_TEST_INSTRUMENT_CATALOG_PATH) {
  return R"({"source_id":)" + std::to_string(source_id) +
         R"(,"instrument_catalog":")" + std::string(catalog) +
         R"(","adapter":"shm","runtime_directory":"/tmp/yquant-test-feed",)"
         R"("stream":"quotes","poll_interval_us":)" +
         std::to_string(interval_ms * 1000U) +
         R"(,"instruments":[{"venue":"SSE","symbol":"600000"}]})";
}

TEST(StrategydProcessTest, HelpExitsSuccessfullyWithoutLoading) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--help"}), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_FALSE(child.output().empty());
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydProcessTest, LoadsTheExampleStrategy) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_EXAMPLE_PATH, "--config",
                         YQUANT_TEST_EXAMPLE_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  EXPECT_NE(child.output().find("lifecycle_strategy: startup_message="
                                "lifecycle strategy started\n"),
            std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
}

TEST(StrategydProcessTest, LifecycleExampleUsesDecodedCustomStartupMessage) {
  testing::ConfigFile config(
      R"({"strategy_id":"lifecycle-custom","parameters":)"
      R"({"startup_message":"\u7b56\u7565: \"custom\" \\ path"}})");
  ASSERT_TRUE(config.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_EXAMPLE_PATH, "--config",
                         config.path()}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  EXPECT_NE(child.output().find("lifecycle_strategy: startup_message="
                                "策略: \"custom\" \\ path\n"),
            std::string::npos);
  EXPECT_NE(child.output().find("strategy_id=lifecycle-custom"),
            std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
}

TEST(StrategydProcessTest, LifecycleExampleAccepts128ByteStartupMessage) {
  const std::string message = std::string(125, 'x') + "策";
  ASSERT_EQ(message.size(), 128U);
  testing::ConfigFile config(
      R"({"strategy_id":"lifecycle-limit","parameters":{"startup_message":")" +
      message + R"("}})");
  ASSERT_TRUE(config.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_EXAMPLE_PATH, "--config",
                         config.path()}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  EXPECT_NE(child.output().find(
                "lifecycle_strategy: startup_message=" + message + '\n'),
            std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
}

TEST(StrategydProcessTest, LifecycleExampleRejectsInvalidStartupParameters) {
  const struct {
    const char* name;
    std::string parameters;
    const char* field;
  } cases[] = {
      {"MissingMessage", "{}", "startup_message"},
      {"MessageWrongType", R"({"startup_message":7})", "startup_message"},
      {"MessageEmpty", R"({"startup_message":""})", "startup_message"},
      {"Message129Bytes",
       R"({"startup_message":")" + std::string(126, 'x') + "策" + R"("})",
       "startup_message"},
      {"MessageContainsNul", R"({"startup_message":"a\u0000b"})",
       "startup_message"},
      {"UnknownParameter", R"({"startup_message":"valid","unexpected":1})",
       "unexpected"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.name);
    testing::ConfigFile config(
        std::string(R"({"strategy_id":"lifecycle-invalid","parameters":)") +
        test_case.parameters + '}');
    ASSERT_TRUE(config.valid());
    ChildProcess child;
    ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_EXAMPLE_PATH, "--config",
                           config.path()}),
              0);
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 1) << child.output();
    EXPECT_NE(child.output().find("stage=initialize"), std::string::npos);
    EXPECT_NE(child.output().find(std::string("parameters.") + test_case.field),
              std::string::npos);
    EXPECT_EQ(child.output().find("Running"), std::string::npos);
    EXPECT_EQ(child.output().find("lifecycle_strategy: startup_message="),
              std::string::npos);
  }
}

TEST(StrategydProcessTest, LoadsTheParameterizedExampleStrategy) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PARAMETERIZED_EXAMPLE_PATH,
                   "--config", YQUANT_TEST_PARAMETERIZED_EXAMPLE_CONFIG_PATH}),
      0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  EXPECT_NE(child.output().find("strategy_id=parameterized-example"),
            std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
}

TEST(StrategydProcessTest, LoadsRelativeToTheWorkingDirectory) {
  std::error_code error;
  const auto working_directory = std::filesystem::current_path(error);
  ASSERT_FALSE(error) << error.message();
  const auto relative_path = std::filesystem::relative(
      YQUANT_TEST_PLUGIN_GOOD_PATH, working_directory, error);
  ASSERT_FALSE(error) << error.message();
  ASSERT_TRUE(relative_path.is_relative());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", relative_path.string(), "--config",
                         YQUANT_TEST_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, LoadsAPathWithSpacesAndNoExtension) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path =
      std::filesystem::path(directory.path()) / "strategy with spaces";
  std::error_code error;
  ASSERT_TRUE(
      std::filesystem::copy_file(YQUANT_TEST_PLUGIN_GOOD_PATH, path, error))
      << error.message();
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", path.string(), "--config",
                         YQUANT_TEST_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, MissingDependencyFailsBeforeCreatingTheStrategy) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = std::filesystem::path(directory.path()) / "strategy.so";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::copy_file(
      YQUANT_TEST_PLUGIN_MISSING_DEPENDENCY_PATH, path, error))
      << error.message();
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", path.string(), "--config",
                         YQUANT_TEST_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=dlopen"), std::string::npos);
  EXPECT_NE(child.output().find(path.string()), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydProcessTest, FailedHandshakeDoesNotTerminateTheTestProcess) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--help"}), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_FALSE(child.ReleaseStart());
}

TEST(StrategydProcessTest, InvalidArgumentsExitWithUsageError) {
  const std::vector<std::vector<std::string>> invalid_arguments = {
      {},
      {"--strategy"},
      {"--strategy", ""},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH},
      {"--config"},
      {"--config", ""},
      {"--config", YQUANT_TEST_CONFIG_PATH},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config", ""},
      {"--unknown"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "unexpected"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--strategy",
       YQUANT_TEST_PLUGIN_GOOD_PATH, "--config", YQUANT_TEST_CONFIG_PATH},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
       YQUANT_TEST_CONFIG_PATH, "--config", YQUANT_TEST_CONFIG_PATH},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
       YQUANT_TEST_CONFIG_PATH, "--market-data-config"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
       YQUANT_TEST_CONFIG_PATH, "--market-data-config", ""},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
       YQUANT_TEST_CONFIG_PATH, "--market-data-config",
       YQUANT_TEST_MARKET_DATA_CONFIG_PATH, "--market-data-config",
       YQUANT_TEST_MARKET_DATA_CONFIG_PATH},
  };
  for (const auto& arguments : invalid_arguments) {
    SCOPED_TRACE(::testing::PrintToString(arguments));
    ChildProcess child;
    ASSERT_EQ(child.Start(arguments), 0);
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 2) << child.output();
    EXPECT_FALSE(child.output().empty());
    EXPECT_TRUE(child.events().empty());
  }
}

class StopSignalTest : public ::testing::TestWithParam<int> {};

TEST_P(StopSignalTest, StopsAndDestroysARealDynamicStrategy) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
                         YQUANT_TEST_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(GetParam()));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

INSTANTIATE_TEST_SUITE_P(PosixSignals, StopSignalTest,
                         ::testing::Values(SIGINT, SIGTERM));

TEST(StrategydProcessTest, StopSignalDuringStartIsHandledAfterStartReturns) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
                         YQUANT_TEST_CONFIG_PATH},
                        true),
            0);
  ASSERT_TRUE(child.WaitForEvent('S')) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.ReleaseStart());
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, LoadAndStartFailuresExitWithRuntimeError) {
  const struct {
    const char* path;
    const char* events;
  } cases[] = {
      {"/yquant-no-such-strategy.so", ""},
      {YQUANT_TEST_PLUGIN_NO_ENTRY_PATH, ""},
      {YQUANT_TEST_PLUGIN_BAD_ABI_PATH, ""},
      {YQUANT_TEST_PLUGIN_CREATE_FAIL_PATH, "C"},
      {YQUANT_TEST_PLUGIN_START_FAIL_PATH, "CPSD"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.path);
    ChildProcess child;
    ASSERT_EQ(child.Start({"--strategy", test_case.path, "--config",
                           YQUANT_TEST_CONFIG_PATH}),
              0);
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 1) << child.output();
    EXPECT_FALSE(child.output().empty());
    ExpectCompletedLifecycle(child.events(), test_case.events);
  }
}

TEST(StrategydProcessTest, StopFailureStillDestroysAndReturnsRuntimeError) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_STOP_FAIL_PATH,
                         "--config", YQUANT_TEST_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, AcceptsConfigBeforeStrategyAndRelativeConfigPath) {
  std::error_code error;
  const auto working_directory = std::filesystem::current_path(error);
  ASSERT_FALSE(error) << error.message();
  const auto config = std::filesystem::relative(YQUANT_TEST_CONFIG_PATH,
                                                working_directory, error);
  ASSERT_FALSE(error) << error.message();
  ChildProcess child;
  ASSERT_EQ(child.Start({"--config", config.string(), "--strategy",
                         YQUANT_TEST_PLUGIN_GOOD_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, AcceptsConfigPathsWithSpaces) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path =
      std::filesystem::path(directory.path()) / "config with spaces";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::copy_file(YQUANT_TEST_CONFIG_PATH, path, error))
      << error.message();
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
                         path.string()}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, ConfigValidationPrecedesLibraryLoading) {
  testing::ConfigFile config(
      R"({"strategy_id":"x","parameters":{},"unexpected":1})");
  ASSERT_TRUE(config.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", "/yquant-no-such-strategy.so",
                         "--config", config.path()}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=config"), std::string::npos);
  EXPECT_NE(child.output().find(config.path()), std::string::npos);
  EXPECT_EQ(child.output().find("stage=dlopen"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydProcessTest, ReportsMissingConfigWithoutCreatingTheStrategy) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
                         "/yquant-no-such-config.json"}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("/yquant-no-such-config.json"),
            std::string::npos);
  EXPECT_NE(child.output().find("stage=config"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydProcessTest, InitializesTypedConfigFromARealJsonFile) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_TYPED_PATH,
                         "--config", YQUANT_TEST_TYPED_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydProcessTest, ParameterErrorsRemainReadableAfterPluginCleanup) {
  testing::ConfigFile config(
      R"({"strategy_id":"bad-parameters","parameters":{"quote_qty":0}})");
  ASSERT_TRUE(config.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_TYPED_PATH,
                         "--config", config.path()}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=initialize"), std::string::npos);
  EXPECT_NE(child.output().find("strategy_id=bad-parameters"),
            std::string::npos);
  EXPECT_NE(child.output().find("parameters.quote_qty"), std::string::npos);
  EXPECT_NE(child.output().find("expected an integer from 1 to 100"),
            std::string::npos);
  ExpectCompletedLifecycle(child.events(), "CPD");
}

TEST(StrategydMarketProcessTest, ContinuesReceivingQuotesUntilSignal) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH, "--config",
                   YQUANT_TEST_QUOTE_CONFIG_PATH, "--market-data-config",
                   YQUANT_TEST_MARKET_DATA_CONFIG_PATH}),
      0);
  ASSERT_TRUE(child.WaitForEventCount('Q', 2)) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectQuoteLifecycle(child.events(), 2);
}

class QuoteObserverSourceTest : public ::testing::TestWithParam<std::uint32_t> {
};

TEST_P(QuoteObserverSourceTest, ReusesStrategyConfigAcrossHostSources) {
  testing::ConfigFile market(HostMarketConfig(1, GetParam()));
  ASSERT_TRUE(market.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--market-data-config", market.path(), "--config",
                         YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH, "--strategy",
                         YQUANT_TEST_QUOTE_EXAMPLE_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("quote_observer: first quote received\n"))
      << child.output();
  EXPECT_NE(child.output().find("Running"), std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  const auto summary = child.output().find("quote_observer: ticks=");
  ASSERT_NE(summary, std::string::npos) << child.output();
  unsigned long long ticks = 0;
  unsigned long long sequence = 0;
  unsigned long long max_age = 0;
  ASSERT_EQ(std::sscanf(child.output().c_str() + summary,
                        "quote_observer: ticks=%llu last_sequence=%llu "
                        "max_age_ns=%llu",
                        &ticks, &sequence, &max_age),
            3);
  EXPECT_GE(ticks, 1U);
  EXPECT_EQ(sequence, ticks);
}

INSTANTIATE_TEST_SUITE_P(HostBinding, QuoteObserverSourceTest,
                         ::testing::Values(1U, 17U));

TEST(StrategydMarketProcessTest, RejectsInProcessMockBeforeLibraryLoading) {
  auto json = HostMarketConfig(1);
  const auto adapter = json.find("\"shm\"");
  ASSERT_NE(adapter, std::string::npos);
  json.replace(adapter, 5, "\"mock\"");
  testing::ConfigFile market(json);
  ASSERT_TRUE(market.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", "/yquant-no-such-strategy.so",
                         "--config", YQUANT_TEST_CONFIG_PATH,
                         "--market-data-config", market.path()}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1);
  EXPECT_NE(child.output().find("stage=market_data_config"), std::string::npos);
  EXPECT_NE(child.output().find("adapter"), std::string::npos);
  EXPECT_EQ(child.output().find("stage=dlopen"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydMarketProcessTest, MarketConfigValidationPrecedesLibraryLoading) {
  testing::ConfigFile market(HostMarketConfig(1, 0));
  ASSERT_TRUE(market.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", "/yquant-no-such-strategy.so",
                         "--config", YQUANT_TEST_CONFIG_PATH,
                         "--market-data-config", market.path()}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=market_data_config"), std::string::npos);
  EXPECT_NE(child.output().find(market.path()), std::string::npos);
  EXPECT_EQ(child.output().find("stage=dlopen"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

struct CatalogFailure {
  const char* name;
  const char* contents;
};

class CatalogFailureProcessTest
    : public ::testing::TestWithParam<CatalogFailure> {};

TEST_P(CatalogFailureProcessTest, RejectsBeforeCreatingOrStartingStrategy) {
  const auto& fault = GetParam();
  testing::ConfigFile catalog(fault.contents);
  ASSERT_TRUE(catalog.valid());
  if (fault.contents[0] == '\0') {
    ASSERT_EQ(unlink(catalog.path()), 0);
  }
  testing::ConfigFile market(HostMarketConfig(1, 1, catalog.path()));
  ASSERT_TRUE(market.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH,
                         "--config", YQUANT_TEST_QUOTE_CONFIG_PATH,
                         "--market-data-config", market.path()}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=market_data_config"), std::string::npos);
  EXPECT_EQ(child.output().find("Running"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

INSTANTIATE_TEST_SUITE_P(
    CatalogValidation, CatalogFailureProcessTest,
    ::testing::Values(
        CatalogFailure{"MissingFile", ""},
        CatalogFailure{"MalformedJson", R"({"instruments":[)"},
        CatalogFailure{"InvalidDefinition",
                       R"({"instruments":[{"venue":"SSE","symbol":"600000",)"
                       R"("price_decimals":7}]})"},
        CatalogFailure{
            "DuplicateDefinition",
            R"({"instruments":[{"venue":"SSE","symbol":"600000",)"
            R"("price_decimals":2},)"
            R"({"venue":"SSE","symbol":"600000","price_decimals":4}]})"},
        CatalogFailure{"MissingDefinition",
                       R"({"instruments":[{"venue":"SZSE","symbol":"000001",)"
                       R"("price_decimals":2}]})"}),
    [](const ::testing::TestParamInfo<CatalogFailure>& info) {
      return info.param.name;
    });

TEST(StrategydMarketProcessTest, MissingMarketConfigDoesNotCreateStrategy) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH, "--config",
                   YQUANT_TEST_QUOTE_CONFIG_PATH, "--market-data-config",
                   "/yquant-no-such-market.json"}),
      0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=market_data_config"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(StrategydMarketProcessTest,
     SignalInterruptsThe60000MillisecondPollInterval) {
  testing::ConfigFile market(HostMarketConfig(60000));
  ASSERT_TRUE(market.valid());
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH,
                         "--config", YQUANT_TEST_QUOTE_CONFIG_PATH,
                         "--market-data-config", market.path()}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGINT));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydMarketProcessTest, StopDuringStartDoesNotDispatchAnyQuote) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH, "--config",
                   YQUANT_TEST_QUOTE_CONFIG_PATH, "--market-data-config",
                   YQUANT_TEST_MARKET_DATA_CONFIG_PATH},
                  true),
      0);
  ASSERT_TRUE(child.WaitForEvent('S')) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.ReleaseStart());
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

TEST(StrategydMarketProcessTest, StartFailureDoesNotDispatchOrStopStrategy) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTE_START_FAIL_PATH,
                         "--config", YQUANT_TEST_QUOTE_CONFIG_PATH,
                         "--market-data-config",
                         YQUANT_TEST_MARKET_DATA_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=start"), std::string::npos);
  ExpectCompletedLifecycle(child.events(), "CPSD");
}

TEST(StrategydMarketProcessTest, TickFailurePairsStopAndRetainsDiagnostic) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PLUGIN_TICK_FAIL_PATH, "--config",
                   YQUANT_TEST_QUOTE_CONFIG_PATH, "--market-data-config",
                   YQUANT_TEST_MARKET_DATA_CONFIG_PATH}),
      0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=tick"), std::string::npos);
  EXPECT_NE(child.output().find("strategy_id=test-quotes"), std::string::npos);
  EXPECT_NE(child.output().find("sequence=1"), std::string::npos);
  ExpectCompletedLifecycle(child.events(), "CPSQTD");
}

TEST(StrategydMarketProcessTest, NoSubscriptionsRemainIdleUntilSignal) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--config",
                         YQUANT_TEST_CONFIG_PATH, "--market-data-config",
                         YQUANT_TEST_MARKET_DATA_CONFIG_PATH}),
            0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CPSTD");
}

struct InheritedSignalCase {
  const char* name;
  InheritedSignals state;
  int signal;
};

class InheritedSignalTest
    : public ::testing::TestWithParam<InheritedSignalCase> {};

TEST_P(InheritedSignalTest, RestoresTerminationHandlingAndStopsNormally) {
  ChildProcess child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_PLUGIN_QUOTES_PATH, "--config",
                   YQUANT_TEST_QUOTE_CONFIG_PATH, "--market-data-config",
                   YQUANT_TEST_MARKET_DATA_CONFIG_PATH},
                  false, GetParam().state),
      0);
  ASSERT_TRUE(child.WaitForEventCount('Q', 2)) << child.output();
  ASSERT_TRUE(child.Signal(GetParam().signal));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectQuoteLifecycle(child.events(), 2);
}

INSTANTIATE_TEST_SUITE_P(
    ExecInheritance, InheritedSignalTest,
    ::testing::Values(InheritedSignalCase{"BlockedSigint",
                                          InheritedSignals::kBlocked, SIGINT},
                      InheritedSignalCase{"BlockedSigterm",
                                          InheritedSignals::kBlocked, SIGTERM},
                      InheritedSignalCase{"IgnoredSigint",
                                          InheritedSignals::kIgnored, SIGINT},
                      InheritedSignalCase{"IgnoredSigterm",
                                          InheritedSignals::kIgnored, SIGTERM}),
    [](const ::testing::TestParamInfo<InheritedSignalCase>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace yquant::strategyd
