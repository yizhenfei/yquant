#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include "yquant/trading/simnow_trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace {
std::string_view Env(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}
}  // namespace

// Read-only live acceptance: drive the normal service loop until account queries
// are available. Instrument arguments bind existing holdings to local IDs.
int main(int argc, char** argv) {
  using namespace std::chrono_literals;
  using namespace yquant::trading;
  if (argc < 2 || argc % 2 != 0) {
    std::fputs("Usage: verify_simnow_account_startup FRONT [EXCHANGE SYMBOL]...\n", stderr);
    return 2;
  }
  SimNowTraderConfig config{argv[1], Env("YQUANT_CTP_BROKER_ID"), Env("YQUANT_CTP_USER_ID"),
      Env("YQUANT_CTP_PASSWORD"), Env("YQUANT_CTP_APP_ID"), Env("YQUANT_CTP_AUTH_CODE"), {}, 45s};
  std::string system_info;
  if (auto* path = std::getenv("YQUANT_CTP_SYSTEM_INFO_FILE")) {
    std::ifstream file(path, std::ios::binary);
    system_info.assign(std::istreambuf_iterator<char>(file), {});
    if (system_info.empty() || file.bad()) return 2;
    config.system_info = system_info;
  }
  for (int i = 2; i < argc; i += 2) {
    config.instruments[{argv[i], argv[i + 1]}] = {static_cast<std::uint32_t>(i / 2)};
  }
  auto adapter = SimNowTradingAdapter::Connect(config);
  TradingService service;
  if (!adapter || !service.BindAccount(1, *adapter)) return 1;
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while (std::chrono::steady_clock::now() < deadline) {
    service.Run();
    FundsSnapshot funds;
    PositionsSnapshot positions;
    if (service.IsAccountReady(1) && service.QueryFunds({1}, funds) &&
        service.QueryPositions({1}, positions)) {
      std::printf("account_ready trading_day=%s positions=%zu\n",
                  adapter->login_status().trading_day.c_str(), positions.positions.size());
      std::fflush(stdout);
      return 0;
    }
    auto login = adapter->login_status();
    auto query = adapter->snapshot_status();
    if (login.phase == LoginPhase::kFailed || login.phase == LoginPhase::kDisconnected ||
        query.phase == SnapshotPhase::kFailed) {
      std::fprintf(stderr, "startup_failed login=%s code=%d snapshot=%s\n",
                   login.reason, login.code, query.reason);
      return 1;
    }
    std::this_thread::sleep_for(1ms);
  }
  std::fputs("startup_timeout\n", stderr);
  return 1;
}
