#ifndef YQUANT_STRATEGY_STRATEGY_SERVICE_H_
#define YQUANT_STRATEGY_STRATEGY_SERVICE_H_

#include "yquant/market/market_client.h"
#include "yquant/model/quote.h"

namespace yquant::strategy {

// Delivers market callbacks to one injected strategy.
// The strategy is borrowed and must outlive this service. Each quote callback
// finishes before control returns to the market service.
class StrategyService final : public market::QuoteListener {
 public:
  explicit StrategyService(market::QuoteListener& strategy)
      : strategy_(strategy) {}
  StrategyService(const StrategyService&) = delete;
  StrategyService& operator=(const StrategyService&) = delete;

  void OnQuote(const model::Quote& quote) override;

 private:
  market::QuoteListener& strategy_;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_SERVICE_H_
