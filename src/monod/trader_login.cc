#include "trader_login.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <thread>
#include <utility>

#include "yquant/trading/trading_service.h"

namespace yquant::monod {
namespace {
std::string_view Env(const char* key) {
  const auto* value = std::getenv(key);
  return value ? value : "";
}
}  // namespace

int RunTraderLogin(int argc, const char* const* argv, bool collect_snapshot,
                   decltype(&trading::SimNowTradingAdapter::Connect) connect) {
  int seconds = 30;
  if (argc >= 2) {
    const std::string_view arg = argv[1];
    const auto end = arg.data() + arg.size();
    const auto [ptr, ec] = std::from_chars(arg.data(), end, seconds);
    if (ec != std::errc{} || ptr != end) seconds = 0;
  }
  const std::string_view symbol = argc == 3 ? argv[2] : "";
  if (argc < 1 || argc > (collect_snapshot ? 3 : 2) ||
      (argc == 3 && symbol.empty()) || seconds <= 0 || seconds > 3600) {
    std::fputs("Usage: monod --simnow-trader-login FRONT [SECONDS]\n"
               "       monod --simnow-startup-snapshot FRONT [SECONDS [SYMBOL]]\n", stderr);
    return 2;
  }
  trading::SimNowTraderConfig config{
      argv[0], Env("YQUANT_CTP_BROKER_ID"), Env("YQUANT_CTP_USER_ID"),
      Env("YQUANT_CTP_PASSWORD"), Env("YQUANT_CTP_APP_ID"),
      Env("YQUANT_CTP_AUTH_CODE"), {}, std::chrono::seconds(seconds)};
  std::array<char, 4096> data{};
  if (const char* path = std::getenv("YQUANT_CTP_SYSTEM_INFO_FILE")) {
    std::ifstream file(path, std::ios::binary);
    file.read(data.data(), data.size());
    if (!file.eof() || file.bad() || !file.gcount()) {
      std::fprintf(stderr, "Invalid terminal system information file\n");
      return 2;
    }
    config.system_info = {data.data(), static_cast<std::size_t>(file.gcount())};
  }
  auto adapter = connect(config);
  trading::TradingService service;
  if (!adapter || !service.BindAccount(1, *adapter)) {
    std::fputs("Invalid trader config or unavailable trader SDK\n", stderr);
    return 1;
  }
  auto previous = trading::LoginPhase::kConnecting;
  bool started = false;
  for (;;) {
    adapter->Run();
    const auto& s = adapter->login_status();
    if (collect_snapshot && s.phase == trading::LoginPhase::kLoggedIn &&
        !started) {
      started = adapter->StartSnapshot(symbol);
      if (!started) {
        std::fputs("Could not start account snapshot query\n", stderr);
        return 1;
      }
    }
    if (started) {
      const auto query = adapter->snapshot_status();
      if (query.phase == trading::SnapshotPhase::kFailed) {
        if (!symbol.empty()) std::fprintf(stderr, "symbol=%s ", argv[2]);
        std::fprintf(stderr, "startup_snapshot_failed reason=%s code=%d\n",
                     query.reason, query.code);
        return 1;
      }
      if (const auto* snapshot = adapter->snapshot()) {
        if (snapshot->terms) {
          const auto& terms = *snapshot->terms;
          std::printf("contract_terms symbol=%s multiplier=%g "
                      "long_margin_rate=%g short_margin_rate=%g "
                      "long_margin_per_lot=%g short_margin_per_lot=%g\n",
                      terms.symbol.c_str(), terms.buy.contract_multiplier,
                      *terms.buy.margin_rate, *terms.sell.margin_rate,
                      terms.buy.margin_per_quantity, terms.sell.margin_per_quantity);
          for (const auto& [offset, name] : {
                   std::pair{trading::Offset::kOpen, "open"},
                   std::pair{trading::Offset::kClose, "close"},
                   std::pair{trading::Offset::kCloseToday, "close_today"}}) {
            const auto& fee = *terms.buy.fees[static_cast<std::size_t>(offset)];
            std::printf("commission offset=%s per_lot=%g per_notional=%g\n",
                        name, fee.per_quantity, fee.per_notional);
          }
        }
        if (!snapshot->holds_error.empty()) {
          std::fprintf(stderr, "startup_holds_unresolved reason=%s\n",
                       snapshot->holds_error.c_str());
        } else {
          std::printf("startup_holds_verified orders=%zu\n", snapshot->holds.size());
        }
        std::printf("startup_snapshot trading_day=%s currencies=%zu "
                    "position_rows=%zu orders=%zu fills=%zu account_ready=false\n",
                    snapshot->trading_day.c_str(), snapshot->balances.size(),
                    snapshot->positions.size(), snapshot->history.orders().size(),
                    snapshot->history.fills().size());
        std::fflush(stdout);
        return snapshot->holds_error.empty() ? 0 : 1;
      }
    } else if (s.phase == trading::LoginPhase::kLoggedIn) {
      const bool ready = service.IsAccountReady(1);
      std::printf("trader_logged_in trading_day=%s account_ready=%s\n",
                  s.trading_day.c_str(), ready ? "true" : "false");
      std::fflush(stdout);
      return ready ? 1 : 0;
    }
    if (s.phase == trading::LoginPhase::kFailed) {
      std::fprintf(stderr, "trader_login_failed reason=%s code=%d\n",
                   s.reason, s.code);
      return 1;
    }
    if (s.phase != previous && s.phase == trading::LoginPhase::kDisconnected) {
      std::fprintf(stderr, "trader_disconnected code=%d\n", s.code);
    }
    previous = s.phase;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace yquant::monod
