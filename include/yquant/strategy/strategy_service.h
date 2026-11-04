#ifndef YQUANT_STRATEGY_STRATEGY_SERVICE_H_
#define YQUANT_STRATEGY_STRATEGY_SERVICE_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "yquant/market/market_client.h"
#include "yquant/strategy/loaded_strategy.h"
#include "yquant/strategy/plugin.h"
#include "yquant/trading/order_id_generator.h"
#include "yquant/trading/trading_client.h"

namespace yquant::strategy {

// Manages strategy instances and dispatches quotes by instrument. Instances
// share this service's order ID generator and the supplied clients.
// Use Load, Stop and destruction between service callbacks on the business thread.
class StrategyService final : public market::QuoteListener {
 public:
  StrategyService(std::uint64_t caller_service_id, market::MarketClient& market,
                  trading::TradingClient& trading);
  ~StrategyService() override;
  StrategyService(const StrategyService&) = delete;
  StrategyService& operator=(const StrategyService&) = delete;

  // Loads all instances before subscribing to their instruments. If creation
  // fails, releases the attempted group and returns false with error set.
  // Returns false for zero or duplicate strategy IDs, an empty group, an active
  // callback or an already loaded group. Stop the current group before reloading.
  bool Load(std::span<const StrategyConfig> configs, std::string& error);
  // Detaches all callbacks and subscriptions before destroying any instance.
  // Returns false from a quote/trading callback, leaving the group running.
  // After Stop succeeds, Load can load another group of strategy instances.
  bool Stop();

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override;

 private:
  // Routes each loaded instance's subscriptions through the service so that
  // instances sharing an instrument use one upstream subscription.
  class SubscriptionClient final : public market::MarketClient {
   public:
    explicit SubscriptionClient(StrategyService& owner) : owner_(owner) {}

    // market::MarketClient implementation.
    void Subscribe(std::uint32_t instrument, market::QuoteListener& listener) override {
      owner_.AddListener(instrument, listener);
    }
    void Unsubscribe(std::uint32_t instrument, market::QuoteListener& listener) override {
      owner_.RemoveListener(instrument, listener);
    }

   private:
    StrategyService& owner_;
  };

  // Records an instance's subscription while the group is being loaded.
  void AddListener(std::uint32_t instrument, market::QuoteListener& listener);
  // Removes the upstream subscription when its last instance unsubscribes.
  void RemoveListener(std::uint32_t instrument, market::QuoteListener& listener);

  market::MarketClient& market_;
  trading::TradingClient& trading_;
  trading::OrderIdGenerator ids_;
  SubscriptionClient subscription_client_;
  std::unordered_map<std::uint32_t, std::vector<market::QuoteListener*>> listeners_;
  std::vector<std::unique_ptr<LoadedStrategy>> strategies_;
  bool active_ = false;
  bool dispatching_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_SERVICE_H_
