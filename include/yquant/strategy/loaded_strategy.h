#ifndef YQUANT_STRATEGY_LOADED_STRATEGY_H_
#define YQUANT_STRATEGY_LOADED_STRATEGY_H_

#include <memory>
#include <vector>

#include "yquant/market/market_client_factory.h"
#include "yquant/strategy/strategy.h"
#include "yquant/trading/trading_client_factory.h"

namespace yquant::strategy {

// Loads a strategy plugin and owns the clients used by its instance.
// Lifecycle operations run on the business thread between service calls.
class LoadedStrategy final {
 public:
  // Loads the plugin and creates clients bound to its instance. Call Start
  // after all instances in the group have loaded successfully.
  // Returns nullptr and sets error if configuration, loading or creation fails.
  static std::unique_ptr<LoadedStrategy> Load(const std::string& config_path,
      const market::MarketClientFactory& markets, std::uint64_t market_id,
      const trading::TradingClientFactory& traders, std::uint64_t trading_id,
      trading::OrderIdGenerator& ids, std::string& error);
  // Loads from parsed settings with the same result and error behavior.
  static std::unique_ptr<LoadedStrategy> Load(const StrategyConfig& config,
      const market::MarketClientFactory& markets, std::uint64_t market_id,
      const trading::TradingClientFactory& traders, std::uint64_t trading_id,
      trading::OrderIdGenerator& ids, std::string& error);
  ~LoadedStrategy();
  LoadedStrategy(const LoadedStrategy&) = delete;
  LoadedStrategy& operator=(const LoadedStrategy&) = delete;
  // Activates the declared quote subscriptions. Returns false if setup fails.
  bool Start();
  // Returns whether both clients can close without interrupting delivery.
  bool CanStop() const;
  std::span<const std::uint32_t> subscriptions() const { return subscriptions_; }
  // Closes both clients, destroys the instance and unloads its library.
  // Returns false if a client cannot close now, without changing the instance.
  // Repeated Stop succeeds.
  bool Stop();
  // Closes both clients without destroying the instance. Returns false if a
  // client cannot close now. Used to close the whole group before any plugin unloads.
  bool Detach();
  const StrategyConfig& config() const { return config_; }

 private:
  LoadedStrategy() = default;
  std::unique_ptr<market::MarketClient> market_;
  std::unique_ptr<trading::TradingClient> trading_;
  StrategyConfig config_;
  void* library_ = nullptr;
  const StrategyPlugin* plugin_ = nullptr;
  Strategy* instance_ = nullptr;
  std::vector<std::uint32_t> subscriptions_;
  bool detached_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_LOADED_STRATEGY_H_
