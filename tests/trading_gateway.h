#ifndef YQUANT_TESTS_TRADING_GATEWAY_H_
#define YQUANT_TESTS_TRADING_GATEWAY_H_

#include <arpa/inet.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "yquant/md/market_data_config.h"
#include "yquant/trading/trading_config.h"

extern char** environ;

namespace yquant::testing {

// Process-test fixture. A broker configuration in strategy test arguments
// starts a real tradingd and is replaced with its generated client config.
// Existing client configs and invalid files pass through to the child
// unchanged.
class TradingGateway {
 public:
  TradingGateway() = default;
  TradingGateway(const TradingGateway&) = delete;
  TradingGateway& operator=(const TradingGateway&) = delete;
  ~TradingGateway() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(3);
      int status;
      while (::waitpid(pid_, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
          ::kill(pid_, SIGKILL);
          while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
          }
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    if (output_) std::fclose(output_);
    if (directory_[0]) {
      std::error_code error;
      std::filesystem::remove_all(directory_, error);
    }
  }

  // Waits on the daemon's flushed readiness output, never a fixed startup
  // delay. All temporary files and process resources remain owned on failure.
  int Prepare(std::vector<std::string>& arguments) {
    const auto trading =
        std::find(arguments.begin(), arguments.end(), "--trading-config");
    const auto market =
        std::find(arguments.begin(), arguments.end(), "--market-data-config");
    if (trading == arguments.end() || trading + 1 == arguments.end() ||
        market == arguments.end() || market + 1 == arguments.end())
      return 0;
    trading::TradingConfigLoader broker;
    md::MarketDataConfigLoader dictionary;
    if (!broker.Load((trading + 1)->c_str()).ok() ||
        !dictionary.Load((market + 1)->c_str()).ok())
      return 0;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return errno;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t size = sizeof(address);
    const bool bound =
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
    const int bind_error = errno;
    ::close(fd);
    if (!bound) return bind_error;
    char pattern[] = "/tmp/yquant-test-gateway-XXXXXX";
    if (!::mkdtemp(pattern)) return errno;
    std::snprintf(directory_, sizeof(directory_), "%s", pattern);
    const auto port = std::to_string(ntohs(address.sin_port));
    const std::string server_path = std::string(directory_) + "/server.json";
    client_path_ = std::string(directory_) + "/client.json";
    const std::string server =
        "{\"listen\":{\"address\":\"127.0.0.1\",\"port\":" + port +
        "},\"market_data_config\":\"" + Escape(*(market + 1)) +
        "\",\"trading_config\":\"" + Escape(*(trading + 1)) +
        "\",\"token\":\"test-gateway\",\"shutdown_timeout_ms\":1000}";
    const std::string client =
        "{\"adapter\":\"tcp\",\"address\":\"127.0.0.1\",\"port\":" + port +
        ",\"client_id\":1,\"token\":\"test-gateway\",\"connect_timeout_ms\":"
        "2000,"
        "\"heartbeat_timeout_ms\":5000,\"shutdown_timeout_ms\":1000}";
    if (!Write(server_path, server) || !Write(client_path_, client)) return EIO;
    output_ = std::tmpfile();
    if (!output_) return errno;
    const int output_fd = ::fileno(output_);
    if (::fcntl(output_fd, F_SETFD, FD_CLOEXEC) < 0) return errno;
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return error;
    error =
        posix_spawn_file_actions_adddup2(&actions, output_fd, STDOUT_FILENO);
    if (!error)
      error =
          posix_spawn_file_actions_adddup2(&actions, output_fd, STDERR_FILENO);
    const char* argv[] = {YQUANT_TEST_TRADINGD_PATH, "--config",
                          server_path.c_str(), nullptr};
    if (!error)
      error = ::posix_spawn(&pid_, argv[0], &actions, nullptr,
                            const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error) {
      pid_ = -1;
      return error;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      char buffer[4096];
      const auto count = ::pread(output_fd, buffer, sizeof(buffer), 0);
      if (count > 0 && std::string_view(buffer, count).find(": Running ") !=
                           std::string_view::npos) {
        *(trading + 1) = client_path_;
        return 0;
      }
      int status;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        return EIO;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return ETIMEDOUT;
  }

 private:
  static std::string Escape(std::string_view text) {
    std::string result;
    for (const char byte : text) {
      if (byte == '\\' || byte == '"') result += '\\';
      result += byte;
    }
    return result;
  }
  static bool Write(const std::string& path, const std::string& text) {
    auto* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool written =
        std::fwrite(text.data(), 1, text.size(), file) == text.size();
    return std::fclose(file) == 0 && written;
  }
  char directory_[64] = {};
  std::string client_path_;
  FILE* output_ = nullptr;
  pid_t pid_ = -1;
};
}  // namespace yquant::testing
#endif  // YQUANT_TESTS_TRADING_GATEWAY_H_
