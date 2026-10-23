#include <stdint.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <system_error>

#include "marketd/config.h"
#include "marketd/runner.h"
#include "yquant/base/format.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/base/termination_signals.h"

namespace {

void PrintUsage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-marketd --config JSON [--check-config]\n"
      "                      [--max-events N]\n"
      "       yquant-marketd --help\n",
      output);
}

// Includes the owned stage detail after adapter resources have been closed.
void PrintFailure(const char* stage, const yquant::base::Status& status,
                  const char* detail) noexcept {
  ::yquant::base::Print(stderr, "yquant-marketd: stage={} code={} detail={}",
                        stage, static_cast<unsigned>(status.code), detail);
  if (status.system_error) {
    ::yquant::base::Print(stderr, " system_error={} ({})",
                          status.system_error.value(),
                          std::strerror(status.system_error.value()));
  }
  std::fputc('\n', stderr);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    PrintUsage(stdout);
    return 0;
  }
  const char* config_path = nullptr;
  bool check_config = false;
  bool has_limit = false;
  uint64_t max_events = 0;
  bool valid = true;
  for (int i = 1; i < argc && valid; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--check-config" && !check_config) {
      check_config = true;
    } else if (arg == "--config" && config_path == nullptr && i + 1 < argc) {
      config_path = argv[++i];
      valid = config_path[0] != '\0' && config_path[0] != '-';
    } else if (arg == "--max-events" && !has_limit && i + 1 < argc) {
      const std::string_view text = argv[++i];
      const auto parsed =
          std::from_chars(text.data(), text.data() + text.size(), max_events);
      valid = parsed.ec == std::errc{} &&
              parsed.ptr == text.data() + text.size() && max_events != 0;
      has_limit = true;
    } else {
      valid = false;
    }
  }
  if (!valid || config_path == nullptr || (check_config && has_limit)) {
    PrintUsage(stderr);
    return 2;
  }
  yquant::marketd::ConfigLoader loader;
  auto status = loader.Load(config_path);
  if (!status.ok()) {
    char detail[768];
    ::yquant::base::FormatTo(detail, sizeof(detail), "{}: {}",
                             loader.error().field_path, loader.error().message);
    PrintFailure("config", status, detail);
    return 1;
  }
  if (check_config) {
    ::yquant::base::Print(stdout, "yquant-marketd: configuration valid\n");
    return 0;
  }
  yquant::base::TerminationSignals signals;
  status = signals.Prepare();
  if (!status.ok()) {
    PrintFailure("signal_setup", status, "cannot prepare termination signals");
    return 1;
  }
  yquant::base::SteadyMonotonicClock clock;
  yquant::marketd::MarketdRunner runner;
  status = runner.Init(loader.config(), clock.Now());
  if (!status.ok()) {
    PrintFailure(runner.error_stage(), status, runner.error_detail());
    return 1;
  }
  ::yquant::base::Print(stdout, "yquant-marketd: Running config={}\n",
                        config_path);
  std::fflush(stdout);
  status = runner.Run(clock, signals, max_events);
  if (!status.ok()) {
    PrintFailure(runner.error_stage(), status, runner.error_detail());
    return 1;
  }
  ::yquant::base::Print(stdout, "yquant-marketd: Stopped events={}\n",
                        runner.event_count());
  return 0;
}
