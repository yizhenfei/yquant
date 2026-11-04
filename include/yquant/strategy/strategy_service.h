#ifndef YQUANT_STRATEGY_STRATEGY_SERVICE_H_
#define YQUANT_STRATEGY_STRATEGY_SERVICE_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "yquant/market/market_client.h"
#include "yquant/strategy/loaded_strategy.h"
#include "yquant/strategy/strategy.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategy {

// Loads and stops a group of strategies sharing one caller's order generator.
// Each instance has its own market and trading clients. Use on the business thread.
class StrategyService final {
 public:
  StrategyService(std::uint64_t caller_service_id,
                  const market::MarketClientFactory& markets, std::uint64_t market_id,
                  const trading::TradingClientFactory& traders, std::uint64_t trading_id);
  ~StrategyService();
  StrategyService(const StrategyService&) = delete;
  StrategyService& operator=(const StrategyService&) = delete;

  // Creates and binds every instance before calling their OnInitialize callbacks.
  // Failure releases the attempted group; an already loaded group is unchanged.
  bool Load(std::span<const StrategyConfig> configs, std::string& error);
  // Closes all clients before destroying any instance. If any client cannot
  // close now, returns false without changing the group. After success another
  // group may load.
  bool Stop();

 private:
  const market::MarketClientFactory& markets_;
  const trading::TradingClientFactory& traders_;
  std::uint64_t market_id_, trading_id_;
  trading::OrderIdGenerator ids_;
  std::vector<std::unique_ptr<LoadedStrategy>> strategies_;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_SERVICE_H_
