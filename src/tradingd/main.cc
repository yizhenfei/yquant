#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

#include "tradingd/config.h"
#include "tradingd/server.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/base/termination_signals.h"

namespace {

void Usage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-tradingd --config JSON [--check-config] "
      "[--initialize-state]\n"
      "       yquant-tradingd --help\n",
      output);
}

void Failure(const char* stage, const yquant::base::Status& status,
             const char* detail) noexcept {
  std::fprintf(stderr, "yquant-tradingd: stage=%s code=%u detail=%s\n", stage,
               static_cast<unsigned>(status.code), detail);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    Usage(stdout);
    return 0;
  }
  const char* path = nullptr;
  bool check = false;
  bool initialize = false;
  bool valid = true;
  for (int i = 1; i < argc && valid; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--config" && path == nullptr && i + 1 < argc) {
      path = argv[++i];
      valid = path[0] != '\0' && path[0] != '-';
    } else if (arg == "--initialize-state" && !initialize) {
      initialize = true;
    } else if (arg == "--check-config" && !check) {
      check = true;
    } else {
      valid = false;
    }
  }
  if (!valid || path == nullptr) {
    Usage(stderr);
    return 2;
  }
  yquant::tradingd::ConfigLoader loader;
  auto status = loader.Load(path);
  if (!status.ok()) {
    char detail[768];
    std::snprintf(detail, sizeof(detail), "%s: %s", loader.error().field_path,
                  loader.error().message);
    Failure("config", status, detail);
    return 1;
  }
  if (check) {
    std::fputs("yquant-tradingd: configuration valid\n", stdout);
    return 0;
  }
  yquant::base::TerminationSignals signals;
  status = signals.Prepare();
  if (!status.ok()) {
    Failure("signal_setup", status, "cannot prepare termination signals");
    return 1;
  }
  yquant::base::SteadyMonotonicClock clock;
  yquant::tradingd::Server server(&clock);
  auto config = loader.config();
  config.persistence.initialize = initialize;
  status = server.Init(config);
  if (!status.ok()) {
    Failure("initialize", status, server.error_detail());
    return 1;
  }
  std::fprintf(stdout, "yquant-tradingd: Running port=%u\n", server.port());
  std::fflush(stdout);
  bool stopping = false;
  for (;;) {
    bool stop = false;
    status = signals.CheckStop(stop);
    if (!status.ok()) {
      Failure("signal_poll", status, "cannot check termination signals");
      server.ReportUnconfirmed();
      return 1;
    }
    const auto now = clock.Now();
    if (stop && !stopping) {
      stopping = true;
      server.BeginStopping(now);
    }
    status = server.Service(now);
    if (!status.ok()) {
      Failure("service", status, server.error_detail());
      server.ReportUnconfirmed();
      return 1;
    }
    if (stopping && server.is_drained()) break;
    std::this_thread::yield();
  }
  status = server.Finish();
  if (!status.ok()) {
    Failure("finalize", status, server.error_detail());
    return 1;
  }
  std::fputs("yquant-tradingd: Stopped\n", stdout);
  return 0;
}
