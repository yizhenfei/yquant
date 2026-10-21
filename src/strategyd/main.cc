#include <cstdio>
#include <cstring>
#include <string_view>

#include "strategyd/strategy_runner.h"
#include "yquant/base/status.h"
#include "yquant/base/termination_signals.h"

namespace {

using yquant::base::ErrorCode;
using yquant::base::Status;
using yquant::base::TerminationSignals;
using yquant::strategyd::LoadAndStart;
using yquant::strategyd::RunnerState;
using yquant::strategyd::StrategyRunner;

void PrintUsage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-strategyd --strategy LIBRARY --config JSON\n"
      "                       [--market-data-config JSON]\n"
      "                       [--trading-config JSON]\n"
      "       yquant-strategyd --help\n",
      output);
}

void PrintFailure(const char* stage, ErrorCode code, const char* detail,
                  int system_error = 0, const char* strategy_path = nullptr,
                  const char* config_path = nullptr,
                  std::string_view strategy_id = {},
                  const char* market_data_path = nullptr,
                  const char* trading_path = nullptr) noexcept {
  std::fprintf(stderr, "yquant-strategyd: stage=%s code=%u", stage,
               static_cast<unsigned>(code));
  if (strategy_path != nullptr) {
    std::fprintf(stderr, " strategy=%s", strategy_path);
  }
  if (config_path != nullptr) {
    std::fprintf(stderr, " config=%s", config_path);
  }
  if (market_data_path != nullptr) {
    std::fprintf(stderr, " market_data_config=%s", market_data_path);
  }
  if (trading_path != nullptr) {
    std::fprintf(stderr, " trading_config=%s", trading_path);
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

void PrintRunnerFailure(const StrategyRunner& runner, const Status& status,
                        const char* strategy_path, const char* config_path,
                        const char* market_data_path,
                        const char* trading_path) noexcept {
  PrintFailure(runner.error_stage(), status.code, runner.error_detail(),
               status.system_error.value(), strategy_path, config_path,
               runner.strategy_id(), market_data_path, trading_path);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintUsage(stdout);
    return 0;
  }
  const char* library_path = nullptr;
  const char* config_path = nullptr;
  const char* market_data_path = nullptr;
  const char* trading_path = nullptr;
  bool valid = argc == 5 || argc == 7 || argc == 9;
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
    } else if (std::strcmp(argv[index], "--market-data-config") == 0 &&
               market_data_path == nullptr) {
      market_data_path = value;
    } else if (std::strcmp(argv[index], "--trading-config") == 0 &&
               trading_path == nullptr) {
      trading_path = value;
    } else {
      valid = false;
    }
  }
  if (!valid || library_path == nullptr || config_path == nullptr ||
      (trading_path != nullptr && market_data_path == nullptr)) {
    PrintUsage(stderr);
    return 2;
  }
  TerminationSignals termination_signals;
  const auto signal_status = termination_signals.Prepare();
  if (!signal_status.ok()) {
    PrintFailure("signal_setup", signal_status.code,
                 "could not prepare termination signals",
                 signal_status.system_error.value());
    return 1;
  }

  StrategyRunner runner(nullptr, nullptr, &termination_signals);
  const Status start_status = runner.Handle(
      LoadAndStart{library_path, config_path, market_data_path, trading_path});
  if (!start_status.ok()) {
    PrintRunnerFailure(runner, start_status, library_path, config_path,
                       market_data_path, trading_path);
    return 1;
  }
  if (runner.state() == RunnerState::kFinished) {
    return 0;
  }

  const auto strategy_id = runner.strategy_id();
  std::fprintf(
      stdout, "yquant-strategyd: Running strategy=%s strategy_id=%.*s\n",
      library_path, static_cast<int>(strategy_id.size()), strategy_id.data());
  std::fflush(stdout);

  const Status run_status = runner.Run(termination_signals);
  if (runner.ignored_quote_count() != 0) {
    std::fprintf(stdout, "yquant-strategyd: ignored_quotes=%llu\n",
                 static_cast<unsigned long long>(runner.ignored_quote_count()));
  }
  if (!run_status.ok()) {
    PrintRunnerFailure(runner, run_status, library_path, config_path,
                       market_data_path, trading_path);
    return 1;
  }
  return 0;
}
