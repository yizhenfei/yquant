#include "trader_login.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <thread>

#include "yquant/trading/trading_service.h"

namespace yquant::monod {
namespace {
std::string_view Env(const char* key) {
  const auto* value = std::getenv(key);
  return value ? value : "";
}
}  // namespace

int RunTraderLogin(int argc, const char* const* argv,
                   decltype(&trading::SimNowTradingAdapter::Connect) connect) {
  int seconds = 30;
  if (argc == 2) {
    const std::string_view arg = argv[1];
    const auto end = arg.data() + arg.size();
    const auto [ptr, ec] = std::from_chars(arg.data(), end, seconds);
    if (ec != std::errc{} || ptr != end) seconds = 0;
  }
  if (argc < 1 || argc > 2 || seconds <= 0 || seconds > 3600) {
    std::fputs("Usage: monod --simnow-trader-login FRONT [SECONDS]\n", stderr);
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
  for (;;) {
    adapter->Run();
    const auto& s = adapter->login_status();
    if (s.phase == trading::LoginPhase::kLoggedIn) {
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
