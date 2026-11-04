#ifndef YQUANT_STRATEGY_STRATEGY_H_
#define YQUANT_STRATEGY_STRATEGY_H_

#include <cstdint>
#include <string>

#include "yquant/strategy/strategy_context.h"

namespace yquant::strategy {

// Defines the quote and trading callbacks implemented by a strategy plugin.
// The host binds this instance to its clients. Submit requests only during
// business callbacks; do not bind listeners or submit during creation/destruction.
class Strategy : public market::QuoteListener, public trading::TradingListener {
 public:
  ~Strategy() override = default;
  // Runs once after both clients are bound, before business callbacks begin.
  // Registers subscriptions through the context's market client. Returns false
  // with error on failure; the host closes the clients and destroys the instance.
  // Do not submit trades or run services during initialization.
  virtual bool OnInitialize(std::string& error) = 0;
};

inline constexpr std::uint32_t kStrategyPluginVersion = 5;

// Defines the plugin version and the functions the host uses to create and
// destroy strategy instances. Host and plugin must use compatible headers,
// compiler ABI and standard library. On failure, create sets error and returns
// nullptr after cleanup. The host calls destroy before unloading the library.
struct StrategyPlugin {
  std::uint32_t version;
  Strategy* (*create)(StrategyContext&, std::string& error);
  void (*destroy)(Strategy*);
};
using StrategyPluginEntry = const StrategyPlugin* (*)();

}  // namespace yquant::strategy

// Plugins export this entry with C linkage and default visibility.
extern "C" __attribute__((visibility("default")))
const yquant::strategy::StrategyPlugin* yquant_strategy_plugin();

#endif  // YQUANT_STRATEGY_STRATEGY_H_
