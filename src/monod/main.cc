#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "market/fixed_quote_source.h"
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
  constexpr std::uint32_t kStrategyInstrumentId = 42;
  const std::array<yquant::model::Quote, 5> quotes{{
      {42, 100.25, 100.5, 7, 11, 1},
      {81, 98.75, 99.25, 5, 8, 2},
      {42, 100.5, 100.75, 9, 13, 3},
      {81, 99, 99.5, 6, 9, 4},
      {42, 100.5, 100.75, 15, 4, 5},
  }};
  yquant::examples::FixedQuoteSource source(quotes);
  PrintIntent output;
  yquant::market::MarketService market(source);
  yquant::market::LocalMarketClient local_client(market);
  yquant::market::MarketClient& client = local_client;
  yquant::examples::QuoteStrategy strategy(output);
  yquant::strategy::StrategyService strategy_service(strategy);
  client.Subscribe(kStrategyInstrumentId, strategy_service);
  yquant::container::Container container;
  const bool completed =
      container.Register(market) &&
      container.Run([&source] { return source.exhausted(); });
  client.Unsubscribe(kStrategyInstrumentId, strategy_service);
  if (!completed) {
    std::fprintf(stderr, "Failed to run the market service\n");
    return 1;
  }
  return 0;
}
