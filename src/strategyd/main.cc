// Runs one configured strategy process. Keeps argument parsing and process
// diagnostics here; configuration ownership and execution belong to the
// assembly and runner.

#include <cstdio>
#include <cstring>
#include <string_view>

#include "strategyd/strategy_assembly.h"
#include "yquant/base/format.h"
#include "yquant/base/status.h"
#include "yquant/base/termination_signals.h"

namespace {

using yquant::base::ErrorCode;
using yquant::base::Status;
using yquant::base::TerminationSignals;
using yquant::strategyd::StrategyAssembly;
using yquant::strategyd::StrategyOptions;
using yquant::strategyd::StrategyRunner;

// Writes CLI syntax to a required, open stream without taking ownership.
void PrintUsage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-strategyd --strategy LIBRARY --config JSON\n"
      "                       --market-data-config JSON\n"
      "                       --trading-config JSON\n"
      "       yquant-strategyd --help\n",
      output);
}

// Writes a failure and available identity/path context to stderr. stage and
// detail are required C strings; null optional paths omit those fields. All
// text is borrowed only for this call; zero omits the system-error field.
void PrintFailure(const char* stage, ErrorCode code, const char* detail,
                  int system_error = 0, const char* strategy_path = nullptr,
                  const char* config_path = nullptr,
                  std::string_view strategy_id = {},
                  const char* market_data_path = nullptr,
                  const char* trading_path = nullptr) noexcept {
  ::yquant::base::Print(stderr, "yquant-strategyd: stage={} code={}", stage,
                        static_cast<unsigned>(code));
  if (strategy_path != nullptr) {
    ::yquant::base::Print(stderr, " strategy={}", strategy_path);
  }
  if (config_path != nullptr) {
    ::yquant::base::Print(stderr, " config={}", config_path);
  }
  if (market_data_path != nullptr) {
    ::yquant::base::Print(stderr, " market_data_config={}", market_data_path);
  }
  if (trading_path != nullptr) {
    ::yquant::base::Print(stderr, " trading_config={}", trading_path);
  }
  if (!strategy_id.empty()) {
    ::yquant::base::Print(
        stderr, " strategy_id={}",
        ::yquant::base::CStringPrefix(strategy_id.data(),
                                      static_cast<int>(strategy_id.size())));
  }
  ::yquant::base::Print(stderr, " detail={}", detail);
  if (system_error != 0) {
    ::yquant::base::Print(stderr, " system_error={} ({})", system_error,
                          std::strerror(system_error));
  }
  std::fputc('\n', stderr);
}

// Reports the runner's stored diagnostic and status. Optional path arguments
// follow PrintFailure's nullability and borrowing rules.
void PrintRunnerFailure(const StrategyRunner& runner, const Status& status,
                        const char* strategy_path, const char* config_path,
                        const char* market_data_path,
                        const char* trading_path) noexcept {
  PrintFailure(runner.error_stage(), status.code, runner.error_detail(),
               status.system_error.value(), strategy_path, config_path,
               runner.strategy_id(), market_data_path, trading_path);
}

}  // namespace

// Returns 0 after help, cancellation, or normal shutdown, 2 for invalid CLI
// arguments, and 1 for setup, startup, or runtime failure.
int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintUsage(stdout);
    return 0;
  }
  const char* library_path = nullptr;
  const char* config_path = nullptr;
  const char* market_data_path = nullptr;
  const char* trading_path = nullptr;
  bool valid = argc == 9;
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
      market_data_path == nullptr || trading_path == nullptr) {
    PrintUsage(stderr);
    return 2;
  }
  // Prepare the runtime environment before loading plugin code. Stop requests
  // are checked at configuration-load and event-loop boundaries.
  TerminationSignals environment;
  const auto signal_status = environment.Prepare();
  if (!signal_status.ok()) {
    PrintFailure("environment_setup", signal_status.code,
                 "could not prepare event-loop environment",
                 signal_status.system_error.value());
    return 1;
  }

  yquant::base::SteadyMonotonicClock clock;
  StrategyAssembly assembly(clock);
  const auto loaded =
      assembly.Load(StrategyOptions{library_path, config_path, market_data_path,
                                    trading_path},
                    environment);
  if (!loaded.ok()) {
    PrintFailure(assembly.error_stage(), loaded.code, assembly.error_detail(),
                 loaded.system_error.value(), library_path, config_path,
                 assembly.strategy_id(), market_data_path, trading_path);
    return 1;
  }
  if (assembly.cancelled()) {
    return 0;
  }
  StrategyRunner runner(clock, environment, assembly.source(),
                        assembly.trading());
  const Status start_status = runner.LoadAndStart(assembly.start_options());
  if (!start_status.ok()) {
    PrintRunnerFailure(runner, start_status, library_path, config_path,
                       market_data_path, trading_path);
    return 1;
  }

  const auto strategy_id = runner.strategy_id();
  ::yquant::base::Print(
      stdout, "yquant-strategyd: Running strategy={} strategy_id={}\n",
      library_path,
      ::yquant::base::CStringPrefix(strategy_id.data(),
                                    static_cast<int>(strategy_id.size())));
  // Make readiness visible immediately to supervising processes.
  std::fflush(stdout);

  const Status run_status = runner.Run();
  if (runner.ignored_quote_count() != 0) {
    ::yquant::base::Print(stdout, "yquant-strategyd: ignored_quotes={}\n",
                          runner.ignored_quote_count());
  }
  if (!run_status.ok()) {
    PrintRunnerFailure(runner, run_status, library_path, config_path,
                       market_data_path, trading_path);
    return 1;
  }
  return 0;
}
