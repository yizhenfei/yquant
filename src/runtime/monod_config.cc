#include "yquant/runtime/monod.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
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

bool Priority(const std::string& value, trading::SimNowClosePriority& priority) {
  if (value == "today") priority = trading::SimNowClosePriority::kTodayFirst;
  else if (value == "yesterday") priority = trading::SimNowClosePriority::kYesterdayFirst;
  else return value == "unspecified";
  return true;
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
  std::uint64_t caller = 0;
  std::map<std::uint64_t, std::map<std::uint64_t, std::string>> groups;
  std::set<std::uint32_t> contracts;
  for (const auto& [key, value] : fields) {
    bool valid = !value.empty();
    if (key == "strategy") strategy_file = value;
    else if (key == "market_front") c.market_front = value;
    else if (key == "trading_front") c.trading_front = value;
    else if (key == "caller_service_id") valid &= Number(value, caller);
    else if (key == "startup_seconds") valid &= Number(value, c.startup_seconds);
    else if (key == "shutdown_seconds") valid &= Number(value, c.shutdown_seconds);
    else if (key == "run_seconds") valid &= Number(value, c.run_seconds);
    else if (key.starts_with("strategy.")) {
      const auto split = key.find('.', 9);
      std::uint64_t service = 0, instance = 0;
      valid &= split != key.npos && Number(key.substr(9, split - 9), service) &&
               Number(key.substr(split + 1), instance) && service && instance;
      if (valid) valid = groups[service].emplace(instance, value).second;
    } else if (key.starts_with("instrument.")) {
      std::uint32_t id = 0;
      std::istringstream entry(value);
      std::string venue, contract, preference, extra;
      trading::SimNowInstrumentBinding binding{};
      valid &= Number(key.substr(11), id) && id &&
               static_cast<bool>(entry >> venue >> contract);
      if (entry >> preference) valid &= Priority(preference, binding.close_priority);
      if (entry >> extra) valid = false;
      binding.instrument_id = id;
      if (valid) {
        valid = contracts.insert(id).second &&
                c.instruments.emplace(std::pair{venue, contract}, binding).second;
      }
    } else valid = false;
    if (!valid) { error = "Invalid runtime configuration key: " + key; return false; }
  }
  if (!c.market_front.starts_with("tcp://") || c.market_front.size() <= 6 ||
      !c.trading_front.starts_with("tcp://") || c.trading_front.size() <= 6 ||
      contracts.empty() || c.startup_seconds <= 0 || c.startup_seconds > 3600 ||
      c.shutdown_seconds <= 0 || c.shutdown_seconds > 3600 || c.run_seconds < 0) {
    error = "Incomplete or invalid runtime configuration";
    return false;
  }
  std::error_code ec;
  const auto base = std::filesystem::absolute(path, ec).parent_path();
  if (ec) { error = "Cannot resolve runtime config: " + ec.message(); return false; }
  if (!strategy_file.empty()) {
    if (!groups.empty() || !caller) {
      error = "Single-strategy and grouped configuration cannot be mixed";
      return false;
    }
    strategy::StrategyConfig instance;
    if (!strategy::ReadStrategyConfig((base / strategy_file).string(), instance, error)) return false;
    c.services.push_back({caller, {std::move(instance)}});
  } else {
    if (groups.empty() || fields.contains("caller_service_id")) {
      error = "Incomplete grouped configuration or conflicting legacy keys";
      return false;
    }
    for (const auto& [service, paths] : groups) {
      StrategyGroupConfig group{service, {}};
      for (const auto& [id, file] : paths) {
        strategy::StrategyConfig instance;
        if (!strategy::ReadStrategyConfig((base / file).string(), instance, error)) return false;
        if (instance.strategy_id != id) {
          error = "Strategy identity does not match its deployment binding";
          return false;
        }
        group.instances.push_back(std::move(instance));
      }
      c.services.push_back(std::move(group));
    }
  }
  c.account_id = c.services.front().instances.front().account_id;
  for (const auto& group : c.services) {
    for (const auto& instance : group.instances) {
      if (instance.account_id != c.account_id) {
        error = "Strategy instances must share the configured trading account";
        return false;
      }
    }
  }
  std::set<std::string> symbols;
  for (const auto& [native, binding] : c.instruments) {
    if (!symbols.insert(native.second).second) {
      error = "Market symbol is ambiguous across exchanges";
      return false;
    }
  }
  output = std::move(c);
  error.clear();
  return true;
}

}  // namespace yquant::runtime
