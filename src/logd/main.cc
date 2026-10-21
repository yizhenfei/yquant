#include <signal.h>
#include <time.h>

#include <charconv>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "yquant/base/logging.h"

namespace {

constexpr timespec kSignalPollInterval{0, 10000000};
volatile sig_atomic_t stop_requested = 0;

void Stop(int) noexcept { stop_requested = 1; }

bool ParseNumber(const char* text, std::uint64_t& value) noexcept {
  const std::string_view input(text);
  const auto result =
      std::from_chars(input.data(), input.data() + input.size(), value);
  return result.ec == std::errc{} && result.ptr == input.data() + input.size();
}

void Usage() noexcept {
  std::fprintf(
      stderr,
      "usage: yquant-logd [--capacity BYTES] [--max-file-bytes BYTES] "
      "--channel QUEUE_FILE LOG_FILE [--channel QUEUE_FILE LOG_FILE ...]"
      "\nOptions apply to all channels and must precede --channel.\n");
}

}  // namespace

int main(int argc, char** argv) {
  using namespace yquant::base;
  if (argc == 2 && std::strcmp(argv[1], "--help") == 0) {
    Usage();
    return 0;
  }
  LoggerConfig config;
  int first_channel = 1;
  while (first_channel < argc &&
         std::strcmp(argv[first_channel], "--channel") != 0) {
    if (first_channel + 1 >= argc) {
      Usage();
      return 2;
    }
    std::uint64_t value = 0;
    if (!ParseNumber(argv[first_channel + 1], value)) {
      Usage();
      return 2;
    }
    if (std::strcmp(argv[first_channel], "--capacity") == 0) {
      config.queue_capacity = value;
    } else if (std::strcmp(argv[first_channel], "--max-file-bytes") == 0) {
      config.max_file_bytes = value;
    } else {
      Usage();
      return 2;
    }
    first_channel += 2;
  }
  if (first_channel == argc || (argc - first_channel) % 3 != 0) {
    Usage();
    return 2;
  }
  for (int index = first_channel; index < argc; index += 3) {
    if (std::strcmp(argv[index], "--channel") != 0) {
      Usage();
      return 2;
    }
  }
  struct sigaction action{};
  action.sa_handler = Stop;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, nullptr) != 0 ||
      sigaction(SIGTERM, &action, nullptr) != 0) {
    std::perror("logd sigaction");
    return 1;
  }
  LogConfig manager_config;
  manager_config.max_loggers =
      static_cast<std::size_t>((argc - first_channel) / 3);
  LogManager manager;
  auto status = manager.Initialize(manager_config);
  if (!status.ok()) {
    std::fprintf(stderr,
                 "logd initialization failed: code=%d system_error=%d\n",
                 static_cast<int>(status.code), status.system_error.value());
    return 1;
  }
  for (int index = first_channel; index < argc; index += 3) {
    config.queue_path = argv[index + 1];
    config.file_path = argv[index + 2];
    status = manager.CreateLogSink(config);
    if (!status.ok()) {
      std::fprintf(stderr, "logd channel %s failed: code=%d system_error=%d\n",
                   argv[index + 1], static_cast<int>(status.code),
                   status.system_error.value());
      (void)manager.Shutdown();
      return 1;
    }
  }
  while (!stop_requested) {
    (void)nanosleep(&kSignalPollInterval, nullptr);
  }
  const auto result = manager.Shutdown();
  if (!result.status.ok()) {
    std::fprintf(stderr,
                 "logd shutdown failed: channel=%zu stage=%d system_error=%d\n",
                 result.channel_index, static_cast<int>(result.stage),
                 result.status.system_error.value());
    return 1;
  }
  return 0;
}
