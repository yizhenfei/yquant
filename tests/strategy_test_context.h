#ifndef YQUANT_TESTS_STRATEGY_TEST_CONTEXT_H_
#define YQUANT_TESTS_STRATEGY_TEST_CONTEXT_H_

#include "yquant/market/local_market_client.h"
#include "yquant/strategy/strategy.h"

namespace yquant::test {

// Supplies an idle market client to tests that deliver quotes directly. Trading
// uses the caller's real client; both clients close before this helper is gone.
class StrategyTestContext {
 public:
  StrategyTestContext(trading::TradingClient& trading, trading::OrderIdGenerator& ids,
                      std::uint64_t account, std::uint64_t strategy)
      : config_{"", strategy, account, {}}, context_(config_, client_, trading, ids) {}
  ~StrategyTestContext() {
    client_.Close();
    context_.trading_client().Close();
  }
  strategy::StrategyContext& context() { return context_; }
  bool Initialize(strategy::Strategy& instance, std::string& error) {
    return client_.BindListener(instance) && context_.trading_client().BindListener(instance) &&
           instance.OnInitialize(error);
  }

 private:
  class IdleSource final : public market::QuoteSource {
   public:
    // market::QuoteSource implementation.
    bool TryRead(model::Quote&) override { return false; }
  } source_;
  strategy::StrategyConfig config_;
  market::MarketService service_{source_};
  market::LocalMarketClient client_{service_};
  strategy::StrategyContext context_;
};

}  // namespace yquant::test

#endif  // YQUANT_TESTS_STRATEGY_TEST_CONTEXT_H_
