#ifndef YQUANT_STRATEGY_STRATEGY_H_
#define YQUANT_STRATEGY_STRATEGY_H_

#include <cstdint>
#include <map>
#include <span>
#include <string>

#include "yquant/market/market_client.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

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

// Defines the quote and trading callbacks implemented by a strategy plugin.
// Submit requests only during business callbacks, using this instance as the
// sole trading listener so the host can detach all callbacks before unloading.
// Creation/destruction must not subscribe or register additional listeners.
class Strategy : public market::QuoteListener, public trading::TradingListener {
 public:
  ~Strategy() override = default;
  // Declares the initial quote subscriptions after creation. The list may be
  // empty; IDs must be nonzero and unique. The host copies the list and manages
  // subscription setup and removal.
  virtual std::span<const std::uint32_t> Subscriptions() const = 0;
};

inline constexpr std::uint32_t kStrategyPluginVersion = 2;

// Defines the plugin version and the functions the host uses to create and
// destroy strategy instances. Host and plugin must use compatible headers,
// compiler ABI and standard library. On failure, create sets error and returns
// nullptr after cleanup. The host calls destroy before unloading the library.
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

#endif  // YQUANT_STRATEGY_STRATEGY_H_
