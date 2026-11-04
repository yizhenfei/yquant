#ifndef YQUANT_STRATEGY_STRATEGY_SERVICE_H_
#define YQUANT_STRATEGY_STRATEGY_SERVICE_H_

#include <memory>
#include <span>

#include "yquant/market/market_client.h"
#include "yquant/strategy/plugin.h"

namespace yquant::strategy {

// Runs a group of strategy instances on one business thread. Managed instances
// share the caller's order generator and borrowed clients, which must outlive
// the service. Load, Stop and destruction occur between service callbacks.
class StrategyService final : public market::QuoteListener {
 public:
  // Forwards quotes to one borrowed strategy, preserving the direct assembly
  // path. The caller owns this strategy and its subscription lifecycle.
  explicit StrategyService(market::QuoteListener& strategy);
  StrategyService(std::uint64_t caller_service_id, market::MarketClient& market,
                  trading::TradingClient& trading);
  ~StrategyService() override;
  StrategyService(const StrategyService&) = delete;
  StrategyService& operator=(const StrategyService&) = delete;

  // Creates an entire group before subscribing. Failure leaves this service
  // empty and releases the attempted group. Rejects duplicate strategy IDs,
  // an empty group, an already loaded group or the borrowed-strategy mode.
  bool Load(std::span<const StrategyConfig> configs, std::string& error);
  // Detaches all callbacks and subscriptions before destroying any instance.
  // Returns false from a quote/trading callback, leaving the group running.
  // After Stop, Load can start another group without resetting order sequences.
  bool Stop();

  // market::QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override;

 private:
  struct Instances;
  market::QuoteListener* borrowed_ = nullptr;
  std::unique_ptr<Instances> instances_;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_SERVICE_H_
