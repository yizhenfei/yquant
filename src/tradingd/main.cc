#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <thread>

#include "tradingd/config.h"
#include "tradingd/server.h"
#include "yquant/base/format.h"
#include "yquant/base/monotonic_clock.h"
#include "yquant/base/termination_signals.h"

namespace {

void Usage(FILE* output) noexcept {
  std::fputs(
      "Usage: yquant-tradingd --config JSON [--check-config]\n"
      "       yquant-tradingd --help\n",
      output);
}

void Failure(const char* stage, const yquant::base::Status& status,
             const char* detail) noexcept {
  ::yquant::base::Print(stderr, "yquant-tradingd: stage={} code={} detail={}\n",
                        stage, static_cast<unsigned>(status.code), detail);
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    Usage(stdout);
    return 0;
  }
  const char* path = nullptr;
  bool check = false;
  bool valid = true;
  for (int i = 1; i < argc && valid; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--config" && path == nullptr && i + 1 < argc) {
      path = argv[++i];
      valid = path[0] != '\0' && path[0] != '-';
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
    ::yquant::base::FormatTo(detail, sizeof(detail), "{}: {}",
                             loader.error().field_path, loader.error().message);
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
  auto server = std::unique_ptr<yquant::tradingd::Server>(
      new (std::nothrow) yquant::tradingd::Server(&clock));
  if (!server) {
    Failure("allocate", {yquant::base::ErrorCode::kResourceExhausted, {}},
            "cannot allocate server");
    return 1;
  }
  status = server->Init(loader.config());
  if (!status.ok()) {
    Failure("initialize", status, server->error_detail());
    return 1;
  }
  ::yquant::base::Print(stdout, "yquant-tradingd: Running port={}\n",
                        server->port());
  std::fflush(stdout);
  bool stopping = false;
  for (;;) {
    bool stop = false;
    status = signals.CheckStop(stop);
    if (!status.ok()) {
      Failure("signal_poll", status, "cannot check termination signals");
      server->ReportUnconfirmed();
      return 1;
    }
    const auto now = clock.Now();
    if (stop && !stopping) {
      stopping = true;
      server->BeginStopping(now);
    }
    status = server->Service(now);
    if (!status.ok()) {
      Failure("service", status, server->error_detail());
      server->ReportUnconfirmed();
      return 1;
    }
    if (stopping && server->is_drained()) {
      break;
    }
    std::this_thread::yield();
  }
  std::fputs("yquant-tradingd: Stopped\n", stdout);
  return 0;
}
