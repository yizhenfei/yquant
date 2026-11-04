#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

#include "yquant/runtime/monod.h"

namespace {
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }
std::string_view Env(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}
}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::puts("Usage: monod --config PATH");
    return 0;
  }
  if (argc != 3 || std::string_view(argv[1]) != "--config") {
    std::fputs("Unsupported argument. Use monod --config PATH.\n", stderr);
    return 2;
  }
  yquant::runtime::MonodConfig config;
  std::string error;
  if (!yquant::runtime::ReadMonodConfig(argv[2], config, error)) {
    std::fprintf(stderr, "%s\n", error.c_str());
    return 2;
  }
  yquant::market::SimNowMarketConfig market{config.market_front, {},
      Env("YQUANT_CTP_BROKER_ID"), Env("YQUANT_CTP_USER_ID"), Env("YQUANT_CTP_PASSWORD"),
      0};
  for (const auto& [native, binding] : config.instruments) {
    market.subscriptions.push_back({native.second, binding.instrument_id});
  }
  yquant::trading::SimNowTraderConfig trader{config.trading_front, market.broker, market.user,
      market.password, Env("YQUANT_CTP_APP_ID"), Env("YQUANT_CTP_AUTH_CODE"), {},
      std::chrono::seconds(config.startup_seconds)};
  trader.instruments = config.instruments;
  trader.confirm_settlement = true;
  std::array<char, 4096> system_info{};
  if (const auto* path = std::getenv("YQUANT_CTP_SYSTEM_INFO_FILE")) {
    std::ifstream file(path, std::ios::binary);
    file.read(system_info.data(), system_info.size());
    if (!file.eof() || file.bad() || !file.gcount()) {
      std::fputs("Invalid terminal system information file\n", stderr);
      return 2;
    }
    trader.system_info = {system_info.data(), static_cast<std::size_t>(file.gcount())};
  }
  std::signal(SIGINT, Stop);
  std::signal(SIGTERM, Stop);
  auto source = yquant::market::SimNowQuoteSource::Connect(market);
  auto adapter = yquant::trading::SimNowTradingAdapter::Connect(trader);
  if (!source || !adapter) {
    std::fputs("Invalid SimNow configuration or unavailable SDK\n", stderr);
    return 2;
  }
  const int rc = yquant::runtime::RunMonod(config, *source, *adapter,
                                         [] { return stopped != 0; }, error);
  if (rc) std::fprintf(stderr, "%s\n", error.c_str());
  return rc;
}
