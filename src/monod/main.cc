#include <cinttypes>
#include <cstdio>

#include "strategies/quote_strategy.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace {

// Writes strategy intents to stdout for the fixed-quote validation run.
class PrintIntent final : public yquant::examples::QuoteIntentListener {
 public:
  void OnQuoteIntent(const yquant::examples::QuoteIntent& intent) override {
    std::printf("quote_intent instrument=%" PRIu32 " bid=%.17g ask=%.17g\n",
                intent.instrument_id, intent.bid_price, intent.ask_price);
  }
};

}  // namespace

int main() {
  PrintIntent output;
  yquant::market::MarketService market;
  yquant::market::LocalMarketClient local_client(market);
  yquant::market::MarketClient& client = local_client;
  yquant::examples::QuoteStrategy strategy(output);
  yquant::strategy::StrategyService strategy_service(strategy);
  client.Subscribe(strategy_service);
  const yquant::model::Quote quote{42, 100.25, 100.5, 7, 11};
  market.Publish(quote);
  return 0;
}
