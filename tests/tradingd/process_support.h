#ifndef YQUANT_TESTS_TRADINGD_PROCESS_SUPPORT_H_
#define YQUANT_TESTS_TRADINGD_PROCESS_SUPPORT_H_

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <string>
#include <string_view>
#include <vector>

#include "market_data_feed.h"

extern char** environ;

namespace yquant::tradingd::testing {

constexpr auto kProcessTimeout = std::chrono::seconds(10);

// Owns one real daemon and a nonblocking combined output pipe. Failed tests
// kill and reap their children, so no failure leaves a feed or writer lock
// live.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status_, 0) == -1 && errno == EINTR) {
      }
    }
    if (output_fd_ >= 0) {
      close(output_fd_);
    }
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  int Start(const char* executable, const std::vector<std::string>& arguments) {
    auto adapted_arguments = arguments;
    const int feed_error = feed_.Prepare(adapted_arguments);
    if (feed_error) {
      return feed_error;
    }
    int output[2];
    if (pipe(output) != 0) {
      return errno;
    }
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), adapted_arguments.begin(),
                   adapted_arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0) {
      error =
          posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
      if (error == 0) {
        error = posix_spawn_file_actions_adddup2(&actions, output[1],
                                                 STDERR_FILENO);
      }
      if (error == 0) {
        error = posix_spawn_file_actions_addclose(&actions, output[0]);
      }
      if (error == 0) {
        error = posix_spawn_file_actions_addclose(&actions, output[1]);
      }
      if (error == 0) {
        error = posix_spawn(&pid_, executable, &actions, nullptr, argv.data(),
                            environ);
      }
      posix_spawn_file_actions_destroy(&actions);
    }
    close(output[1]);
    if (error != 0) {
      close(output[0]);
      pid_ = -1;
      return error;
    }
    output_fd_ = output[0];
    if (fcntl(output_fd_, F_SETFL, O_NONBLOCK) != 0) {
      return errno;
    }
    if (fcntl(output_fd_, F_SETFD, FD_CLOEXEC) != 0) {
      return errno;
    }
    return 0;
  }

  bool WaitForOutput(std::string_view text) {
    return ObserveUntil(
        [&] { return output_.find(text) != std::string::npos; });
  }
  bool Wait() {
    return ObserveUntil([&] { return pid_ < 0; });
  }
  bool Signal(int signal) const { return pid_ > 0 && kill(pid_, signal) == 0; }
  int exit_code() const {
    return exited_ && WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }
  const std::string& output() const { return output_; }

 private:
  // Reads only currently available bytes and retries interrupted reads.
  void Drain() {
    char buffer[1024];
    for (;;) {
      const auto count = read(output_fd_, buffer, sizeof(buffer));
      if (count > 0) {
        output_.append(buffer, static_cast<size_t>(count));
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        return;
      }
    }
  }

  // Observes process exit and output under a single deadline per expectation.
  template <typename Predicate>
  bool ObserveUntil(Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
    for (;;) {
      Drain();
      if (pid_ > 0) {
        const auto result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          exited_ = true;
          Drain();
        } else if (result < 0 && errno != EINTR) {
          return false;
        }
      }
      if (output_.find("Running") != std::string::npos) {
        feed_.Release();
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
      pollfd descriptor{output_fd_, POLLIN, 0};
      if (poll(&descriptor, 1, static_cast<int>(remaining.count())) < 0 &&
          errno != EINTR) {
        return false;
      }
    }
  }

  ::yquant::testing::MarketDataFeed feed_;
  pid_t pid_ = -1;
  int output_fd_ = -1;
  int status_ = 0;
  bool exited_ = false;
  std::string output_;
};

}  // namespace yquant::tradingd::testing
#endif  // YQUANT_TESTS_TRADINGD_PROCESS_SUPPORT_H_
