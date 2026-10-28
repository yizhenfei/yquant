#include <array>
#include <cinttypes>
#include <cstdio>

#include "market/fixed_quote_source.h"
#include "market/quote_publisher.h"
#include "strategies/quote_strategy.h"
#include "yquant/container/container.h"
#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/model/quote.h"
#include "yquant/strategy/strategy_service.h"

namespace {

// Writes each intent and its source sequence to stdout for validation.
class PrintIntent final : public yquant::examples::QuoteIntentListener {
 public:
  void OnQuoteIntent(const yquant::examples::QuoteIntent& intent) override {
    std::printf("quote_intent instrument=%" PRIu32
                " sequence=%" PRIu64 " bid=%.17g ask=%.17g\n",
                intent.instrument_id, intent.sequence, intent.bid_price,
                intent.ask_price);
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
  const std::array<yquant::model::Quote, 3> quotes{{
      {42, 100.25, 100.5, 7, 11, 1},
      {42, 100.5, 100.75, 9, 13, 2},
      {42, 100.5, 100.75, 15, 4, 3},
  }};
  yquant::examples::FixedQuoteSource source(quotes);
  yquant::examples::QuotePublisher publisher(source, market);
  yquant::container::Container container;
  if (!container.Register(publisher) || !container.Run()) {
    std::fprintf(stderr, "Failed to run the quote publisher\n");
    return 1;
  }
  return 0;
}
