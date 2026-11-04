#ifndef YQUANT_STRATEGY_STRATEGY_CONFIG_H_
#define YQUANT_STRATEGY_STRATEGY_CONFIG_H_

#include <cstdint>
#include <map>
#include <string>

namespace yquant::strategy {

// Configures one strategy instance and its plugin parameters.
struct StrategyConfig {
  std::string library;
  std::uint64_t strategy_id = 0, account_id = 0;
  std::map<std::string, std::string> parameters;
};

// Reads key=value lines. Unknown or repeated keys and missing required fields
// fail with a diagnostic in error; output remains unchanged on failure.
// Resolves relative library paths against the configuration file's directory.
bool ReadStrategyConfig(const std::string& path, StrategyConfig& output,
                        std::string& error);

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_CONFIG_H_
