#include "yquant/runtime/monod.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <string_view>
#include <utility>

namespace yquant::runtime {
namespace {

std::string Trim(std::string_view s) {
  const auto a = s.find_first_not_of(" \t\r");
  return a == s.npos ? "" : std::string(s.substr(a, s.find_last_not_of(" \t\r") - a + 1));
}
template <typename T>
bool Number(const std::string& s, T& n) {
  const auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), n);
  return ec == std::errc{} && p == s.data() + s.size();
}

}  // namespace

bool ReadMonodConfig(const std::string& path, MonodConfig& output, std::string& error) {
  std::ifstream file(path);
  std::map<std::string, std::string> fields;
  std::string line;
  while (std::getline(file, line)) {
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    const auto pos = line.find('=');
    if (pos == line.npos || line.find('\0') != line.npos ||
        !fields.emplace(Trim(line.substr(0,pos)), Trim(line.substr(pos+1))).second) {
      error = "Invalid or duplicate runtime configuration entry";
      return false;
    }
  }
  if (!file.eof() || file.bad()) { error = "Cannot read runtime configuration"; return false; }
  MonodConfig c;
  std::string strategy_file;
  for (const auto& [key, value] : fields) {
    bool valid = !value.empty();
    if (key == "strategy") strategy_file = value;
    else if (key == "market_front") c.market_front = value;
    else if (key == "trading_front") c.trading_front = value;
    else if (key == "exchange") c.exchange = value;
    else if (key == "symbol") c.symbol = value;
    else if (key == "caller_service_id") valid &= Number(value, c.caller_service_id);
    else if (key == "startup_seconds") valid &= Number(value, c.startup_seconds);
    else if (key == "shutdown_seconds") valid &= Number(value, c.shutdown_seconds);
    else if (key == "run_seconds") valid &= Number(value, c.run_seconds);
    else if (key == "close_priority") {
      if (value == "today") c.close_priority = trading::SimNowClosePriority::kTodayFirst;
      else if (value == "yesterday") c.close_priority = trading::SimNowClosePriority::kYesterdayFirst;
      else valid = value == "unspecified";
    } else valid = false;
    if (!valid) { error = "Invalid runtime configuration key: " + key; return false; }
  }
  if (!c.market_front.starts_with("tcp://") || c.market_front.size() <= 6 ||
      !c.trading_front.starts_with("tcp://") || c.trading_front.size() <= 6 ||
      c.exchange.empty() || c.symbol.empty() || !c.caller_service_id ||
      strategy_file.empty() || c.startup_seconds <= 0 || c.startup_seconds > 3600 ||
      c.shutdown_seconds <= 0 || c.shutdown_seconds > 3600 || c.run_seconds < 0) {
    error = "Incomplete or invalid runtime configuration";
    return false;
  }
  std::error_code ec;
  const auto base = std::filesystem::absolute(path, ec).parent_path();
  if (ec) { error = "Cannot resolve runtime config: " + ec.message(); return false; }
  if (!strategy::ReadStrategyConfig((base / strategy_file).string(), c.strategy, error)) return false;
  output = std::move(c);
  error.clear();
  return true;
}

}  // namespace yquant::runtime
