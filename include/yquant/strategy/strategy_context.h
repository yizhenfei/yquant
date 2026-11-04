#ifndef YQUANT_STRATEGY_STRATEGY_CONTEXT_H_
#define YQUANT_STRATEGY_STRATEGY_CONTEXT_H_

#include "yquant/market/market_client.h"
#include "yquant/strategy/strategy_config.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategy {

// Provides one instance's configuration and clients, plus its service's shared
// order ID generator. The host keeps the context and these borrowed objects
// alive until the instance is destroyed. Use only on the business thread.
class StrategyContext {
 public:
  StrategyContext(const StrategyConfig& config, market::MarketClient& market,
                  trading::TradingClient& trading, trading::OrderIdGenerator& ids)
      : config_(config), market_(market), trading_(trading), ids_(ids) {}

  const StrategyConfig& config() const { return config_; }
  market::MarketClient& market_client() const { return market_; }
  trading::TradingClient& trading_client() const { return trading_; }
  trading::OrderIdGenerator& order_ids() const { return ids_; }

 private:
  const StrategyConfig& config_;
  market::MarketClient& market_;
  trading::TradingClient& trading_;
  trading::OrderIdGenerator& ids_;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_CONTEXT_H_
