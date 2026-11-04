#ifndef YQUANT_STRATEGY_PLUGIN_H_
#define YQUANT_STRATEGY_PLUGIN_H_

#include <cstdint>
#include <map>
#include <string>

#include "yquant/market/market_client.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategy {

// Supplies one strategy instance's immutable settings. Library paths are
// resolved relative to the configuration file; parameter values remain strings.
struct StrategyConfig {
  std::string library;
  std::uint64_t strategy_id = 0, account_id = 0;
  std::uint32_t instrument_id = 0;
  std::map<std::string, std::string> parameters;
};

// Reads key=value lines. Unknown or repeated keys and missing required fields
// fail with a diagnostic; output remains unchanged on failure.
bool ReadStrategyConfig(const std::string& path, StrategyConfig& output,
                        std::string& error);

// A loaded strategy uses this object as its sole trading listener. It receives
// quotes from the host and submits requests only during business callbacks.
// Creation/destruction must not subscribe or register additional listeners.
class Strategy : public market::QuoteListener, public trading::TradingListener {
 public:
  ~Strategy() override = default;
};

inline constexpr std::uint32_t kStrategyPluginVersion = 1;

// The C entry point returns this descriptor before any instance is created.
// This version uses the project's C++ interfaces: host and plugin must share
// the same headers, compiler ABI and standard library. Factory errors return
// nullptr after releasing partial resources; destroy runs before dlclose.
struct StrategyPlugin {
  std::uint32_t version;
  Strategy* (*create)(const StrategyConfig&, trading::TradingClient&,
                     trading::OrderIdGenerator&, std::string& error);
  void (*destroy)(Strategy*);
};
using StrategyPluginEntry = const StrategyPlugin* (*)();

}  // namespace yquant::strategy

// Plugins export this entry with C linkage and default visibility.
extern "C" __attribute__((visibility("default")))
const yquant::strategy::StrategyPlugin* yquant_strategy_plugin();

#endif  // YQUANT_STRATEGY_PLUGIN_H_
