#include <pthread.h>
#include <signal.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "strategyd/strategy_runtime.h"
#include "yquant/base/status.h"

namespace {

using yquant::base::ErrorCode;
using yquant::base::Status;
using yquant::strategyd::LoadAndStart;
using yquant::strategyd::Stop;
using yquant::strategyd::StrategyRuntime;

void PrintUsage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-strategyd --strategy LIBRARY --config JSON\n"
      "       yquant-strategyd --help\n",
      output);
}

void PrintFailure(const char* stage, ErrorCode code, const char* detail,
                  int system_error = 0, const char* strategy_path = nullptr,
                  const char* config_path = nullptr,
                  std::string_view strategy_id = {}) noexcept {
  std::fprintf(stderr, "yquant-strategyd: stage=%s code=%u", stage,
               static_cast<unsigned>(code));
  if (strategy_path != nullptr) {
    std::fprintf(stderr, " strategy=%s", strategy_path);
  }
  if (config_path != nullptr) {
    std::fprintf(stderr, " config=%s", config_path);
  }
  if (!strategy_id.empty()) {
    std::fprintf(stderr, " strategy_id=%.*s",
                 static_cast<int>(strategy_id.size()), strategy_id.data());
  }
  std::fprintf(stderr, " detail=%s", detail);
  if (system_error != 0) {
    std::fprintf(stderr, " system_error=%d (%s)", system_error,
                 std::strerror(system_error));
  }
  std::fputc('\n', stderr);
}

void PrintRuntimeFailure(const StrategyRuntime& runtime, const Status& status,
                         const char* strategy_path,
                         const char* config_path) noexcept {
  PrintFailure(runtime.error_stage(), status.code, runtime.error_detail(),
               status.system_error.value(), strategy_path, config_path,
               runtime.strategy_id());
}

bool PrepareTerminationSignals(sigset_t& signals) noexcept {
  if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
      sigaddset(&signals, SIGTERM) != 0) {
    const int error = errno;
    PrintFailure("signal_setup", ErrorCode::kIoError,
                 "could not prepare termination signal set", error);
    return false;
  }

  // Block before loading user code so any threads it creates inherit the mask.
  const int mask_error = pthread_sigmask(SIG_BLOCK, &signals, nullptr);
  if (mask_error != 0) {
    PrintFailure("signal_setup", ErrorCode::kIoError,
                 "could not block termination signals", mask_error);
    return false;
  }

  // An ignored disposition can survive exec. Reset it while signals are
  // blocked so sigwait can receive these signals without an async handler.
  struct sigaction action{};
  action.sa_handler = SIG_DFL;
  if (sigemptyset(&action.sa_mask) != 0 ||
      sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    const int error = errno;
    PrintFailure("signal_setup", ErrorCode::kIoError,
                 "could not reset termination signal dispositions", error);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintUsage(stdout);
    return 0;
  }
  const char* library_path = nullptr;
  const char* config_path = nullptr;
  bool valid = argc == 5;
  for (int index = 1; valid && index < argc; index += 2) {
    const char* value = argv[index + 1];
    if (value[0] == '\0' || value[0] == '-') {
      valid = false;
    } else if (std::strcmp(argv[index], "--strategy") == 0 &&
               library_path == nullptr) {
      library_path = value;
    } else if (std::strcmp(argv[index], "--config") == 0 &&
               config_path == nullptr) {
      config_path = value;
    } else {
      valid = false;
    }
  }
  if (!valid || library_path == nullptr || config_path == nullptr) {
    PrintUsage(stderr);
    return 2;
  }
  sigset_t termination_signals;
  if (!PrepareTerminationSignals(termination_signals)) {
    return 1;
  }

  StrategyRuntime runtime;
  const Status start_status =
      runtime.Handle(LoadAndStart{library_path, config_path});
  if (!start_status.ok()) {
    PrintRuntimeFailure(runtime, start_status, library_path, config_path);
    return 1;
  }

  const auto strategy_id = runtime.strategy_id();
  std::fprintf(
      stderr, "yquant-strategyd: Running strategy=%s strategy_id=%.*s\n",
      library_path, static_cast<int>(strategy_id.size()), strategy_id.data());
  std::fflush(stderr);

  int exit_code = 0;
  int signal_number = 0;
  const int wait_error = sigwait(&termination_signals, &signal_number);
  if (wait_error != 0) {
    PrintFailure("signal_wait", ErrorCode::kIoError,
                 "could not wait for termination signal", wait_error,
                 library_path, config_path, strategy_id);
    exit_code = 1;
  }

  // Keep the mask until process exit, including throughout OnStop. A wait
  // failure still requires normal strategy cleanup.
  const Status stop_status = runtime.Handle(Stop{});
  if (!stop_status.ok()) {
    PrintRuntimeFailure(runtime, stop_status, library_path, config_path);
    exit_code = 1;
  }
  return exit_code;
}
