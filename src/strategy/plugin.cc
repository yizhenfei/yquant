#include "yquant/strategy/plugin.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <set>
#include <string_view>

namespace yquant::strategy {
namespace {

std::string Trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r");
  return first == value.npos ? "" : std::string(value.substr(
      first, value.find_last_not_of(" \t\r") - first + 1));
}

template <typename T>
bool Id(const std::string& value, T& result) {
  const auto end = value.data() + value.size();
  const auto [ptr, ec] = std::from_chars(value.data(), end, result);
  return ec == std::errc{} && ptr == end && result != 0;
}

}  // namespace

bool ReadStrategyConfig(const std::string& path, StrategyConfig& output, std::string& error) {
  std::ifstream file(path);
  if (!file) { error = "Cannot read strategy config: " + path; return false; }
  StrategyConfig config;
  std::set<std::string> keys;
  std::string line;
  std::size_t number = 0;
  while (std::getline(file, line)) {
    ++number;
    line = Trim(line);
    if (line.empty() || line.front() == '#') continue;
    const auto split = line.find('=');
    const auto key = Trim(line.substr(0, split));
    const auto value = split == line.npos ? "" : Trim(line.substr(split + 1));
    bool valid = !value.empty() && keys.insert(key).second &&
                 line.find('\0') == line.npos;
    if (valid) {
      if (key == "library") config.library = value;
      else if (key == "strategy_id") valid = Id(value, config.strategy_id);
      else if (key == "account_id") valid = Id(value, config.account_id);
      else if (key == "instrument_id") valid = Id(value, config.instrument_id);
      else if (key.starts_with("param.") && key.size() > 6) {
        config.parameters.emplace(key.substr(6), value);
      } else valid = false;
    }
    if (!valid) {
      error = "Invalid strategy config line " + std::to_string(number) + ": " + key;
      return false;
    }
  }
  if (file.bad() || config.library.empty() || !config.strategy_id ||
      !config.account_id || !config.instrument_id) {
    error = "Incomplete strategy config: " + path;
    return false;
  }
  std::error_code ec;
  const auto base = std::filesystem::absolute(path, ec).parent_path();
  if (ec) { error = "Cannot resolve strategy config: " + ec.message(); return false; }
  config.library = (base / config.library).lexically_normal().string();
  output = std::move(config);
  error.clear();
  return true;
}

}  // namespace yquant::strategy
