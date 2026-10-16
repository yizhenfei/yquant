#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "gtest/gtest.h"

extern char** environ;

namespace yquant::strategyd {
namespace {

constexpr auto kProcessTimeout = std::chrono::seconds(10);

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
  if (setsockopt(fds[1], SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)) != 0) {
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

  int Start(const std::vector<std::string>& arguments,
            bool block_start = false) {
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
    argv_storage.insert(argv_storage.end(), arguments.begin(), arguments.end());
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
      error = posix_spawn_file_actions_adddup2(&actions, output[1],
                                             STDOUT_FILENO);
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
        error = posix_spawn(&pid_, YQUANT_TEST_STRATEGYD_PATH, &actions,
                            nullptr, argv.data(), env.data());
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

  bool Wait() { return ObserveUntil([&] { return pid_ < 0; }); }

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
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_EXAMPLE_PATH}), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
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
  ASSERT_EQ(child.Start({"--strategy", relative_path.string()}), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CSTD");
}

TEST(StrategydProcessTest, LoadsAPathWithSpacesAndNoExtension) {
  TemporaryDirectory directory;
  ASSERT_TRUE(directory.valid());
  const auto path = std::filesystem::path(directory.path()) /
                    "strategy with spaces";
  std::error_code error;
  ASSERT_TRUE(std::filesystem::copy_file(YQUANT_TEST_PLUGIN_GOOD_PATH, path,
                                        error))
      << error.message();
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", path.string()}), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CSTD");
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
  ASSERT_EQ(child.Start({"--strategy", path.string()}), 0);
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
      {"--unknown"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "unexpected"},
      {"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH, "--strategy",
       YQUANT_TEST_PLUGIN_GOOD_PATH},
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
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH}), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(GetParam()));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CSTD");
}

INSTANTIATE_TEST_SUITE_P(PosixSignals, StopSignalTest,
                         ::testing::Values(SIGINT, SIGTERM));

TEST(StrategydProcessTest, StopSignalDuringStartIsHandledAfterStartReturns) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_GOOD_PATH}, true), 0);
  ASSERT_TRUE(child.WaitForEvent('S')) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.ReleaseStart());
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  ExpectCompletedLifecycle(child.events(), "CSTD");
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
      {YQUANT_TEST_PLUGIN_START_FAIL_PATH, "CSD"},
  };
  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.path);
    ChildProcess child;
    ASSERT_EQ(child.Start({"--strategy", test_case.path}), 0);
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 1) << child.output();
    EXPECT_FALSE(child.output().empty());
    ExpectCompletedLifecycle(child.events(), test_case.events);
  }
}

TEST(StrategydProcessTest, StopFailureStillDestroysAndReturnsRuntimeError) {
  ChildProcess child;
  ASSERT_EQ(child.Start({"--strategy", YQUANT_TEST_PLUGIN_STOP_FAIL_PATH}), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  ExpectCompletedLifecycle(child.events(), "CSTD");
}

}  // namespace
}  // namespace yquant::strategyd
