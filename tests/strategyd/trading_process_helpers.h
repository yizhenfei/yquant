#ifndef YQUANT_TESTS_STRATEGYD_TRADING_PROCESS_HELPERS_H_
#define YQUANT_TESTS_STRATEGYD_TRADING_PROCESS_HELPERS_H_

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "market_data_feed.h"
#include "trading_gateway.h"

extern char** environ;

namespace yquant::strategyd::trading_testing {

// Owns one real strategyd child and its nonblocking output/event channels.
// Failed assertions still kill/reap the child; waits are bounded and do not
// infer state from a fixed sleep. Test callbacks use the inherited event FD.
class Process {
 public:
  Process() = default;
  Process(const Process&) = delete;
  Process& operator=(const Process&) = delete;
  ~Process() {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status_, 0) == -1 && errno == EINTR) {
      }
    }
    Close(output_fd_);
    Close(event_fd_);
  }

  // Spawns strategyd with copied arguments/environment. On error owns no child
  // and retains only descriptors reclaimed by the destructor.
  int Start(const std::vector<std::string>& arguments) {
    auto adapted_arguments = arguments;
    const int feed_error = feed_.Prepare(adapted_arguments);
    if (feed_error) return feed_error;
    const int gateway_error = gateway_.Prepare(adapted_arguments);
    if (gateway_error) return gateway_error;
    int output[2] = {-1, -1};
    int events[2] = {-1, -1};
    if (pipe(output) != 0 || pipe(events) != 0) {
      const int error = errno;
      for (int fd : {output[0], output[1], events[0], events[1]}) Close(fd);
      return error;
    }
    std::vector<std::string> storage{YQUANT_TEST_STRATEGYD_PATH};
    storage.insert(storage.end(), adapted_arguments.begin(),
                   adapted_arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    std::vector<std::string> env_storage;
    for (char** entry = environ; *entry != nullptr; ++entry) {
      if (std::strncmp(*entry, "YQUANT_TEST_EVENT_FD=", 21) != 0) {
        env_storage.emplace_back(*entry);
      }
    }
    env_storage.push_back("YQUANT_TEST_EVENT_FD=" + std::to_string(events[1]));
    std::vector<char*> env;
    for (auto& entry : env_storage) env.push_back(entry.data());
    env.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0) {
      error =
          posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
      if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, output[1],
                                                 STDERR_FILENO);
      for (int fd : {output[0], output[1], events[0]}) {
        if (error == 0) error = posix_spawn_file_actions_addclose(&actions, fd);
      }
      if (error == 0)
        error = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(),
                            env.data());
      posix_spawn_file_actions_destroy(&actions);
    }
    Close(output[1]);
    Close(events[1]);
    output_fd_ = output[0];
    event_fd_ = events[0];
    if (error != 0) {
      pid_ = -1;
      return error;
    }
    if (fcntl(output_fd_, F_SETFL, O_NONBLOCK) == -1 ||
        fcntl(event_fd_, F_SETFL, O_NONBLOCK) == -1)
      return errno;
    return 0;
  }

  bool WaitForOutput(std::string_view marker) {
    return Observe([&] { return output_.find(marker) != std::string::npos; });
  }
  bool Wait() {
    return Observe([&] { return pid_ < 0; });
  }
  bool Signal(int signal) { return pid_ > 0 && kill(pid_, signal) == 0; }
  int exit_code() const {
    return exited_ && WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }
  const std::string& output() const { return output_; }
  const std::string& events() const { return events_; }

 private:
  static void Close(int fd) {
    if (fd >= 0) close(fd);
  }
  static void Drain(int fd, std::string& out) {
    char bytes[512];
    for (;;) {
      const auto count = read(fd, bytes, sizeof(bytes));
      if (count > 0)
        out.append(bytes, static_cast<std::size_t>(count));
      else if (count == -1 && errno == EINTR)
        continue;
      else
        return;
    }
  }

  template <typename Predicate>
  bool Observe(Predicate done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      Drain(output_fd_, output_);
      Drain(event_fd_, events_);
      if (pid_ > 0) {
        const auto result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          exited_ = true;
          Drain(output_fd_, output_);
          Drain(event_fd_, events_);
        } else if (result == -1 && errno != EINTR)
          return false;
      }
      if (output_.find("Running") != std::string::npos) feed_.Release();
      if (done()) return true;
      if (pid_ < 0) return false;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count();
      if (remaining <= 0) return false;
      pollfd fds[] = {{output_fd_, POLLIN, 0}, {event_fd_, POLLIN, 0}};
      if (poll(fds, 2, static_cast<int>(remaining)) == -1 && errno != EINTR)
        return false;
    }
  }

  ::yquant::testing::MarketDataFeed feed_;
  ::yquant::testing::TradingGateway gateway_;
  pid_t pid_ = -1;
  int output_fd_ = -1;
  int event_fd_ = -1;
  int status_ = 0;
  bool exited_ = false;
  std::string output_;
  std::string events_;
};

}  // namespace yquant::strategyd::trading_testing

#endif  // YQUANT_TESTS_STRATEGYD_TRADING_PROCESS_HELPERS_H_
