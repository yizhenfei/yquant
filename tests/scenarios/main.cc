#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "funds_demo.h"
#include "funds_lifecycle_demo.h"
#include "maker_demo.h"
#include "market/fixed_quote_source.h"
#include "position_holds_demo.h"
#include "positions_demo.h"
#include "reservation_demo.h"
#include "strategies/quote_strategy.h"
#include "startup_demo.h"
#include "trading_demo.h"
#include "yquant/container/container.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/model/quote.h"

namespace {

// Writes each intent and its source sequence to stdout for validation.
class PrintIntent final : public yquant::examples::QuoteIntentListener {
 public:
  // yquant::examples::QuoteIntentListener implementation.
  void OnQuoteIntent(const yquant::examples::QuoteIntent& intent) override {
    std::printf("quote_intent instrument=%" PRIu32
                " sequence=%" PRIu64 " bid=%.17g ask=%.17g\n",
                intent.instrument_id, intent.sequence, intent.bid_price,
                intent.ask_price);
  }
};

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--maker") {
    return yquant::verification::RunMakerDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--orders") {
    return yquant::verification::RunTradingDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--cancel") {
    return yquant::verification::RunTradingDemo(
        yquant::verification::TradingScenario::kCancel);
  }
  if (argc == 2 && std::string_view(argv[1]) == "--fills") {
    return yquant::verification::RunTradingDemo(yquant::verification::TradingScenario::kFill);
  }
  if (argc == 2 && std::string_view(argv[1]) == "--funds") {
    return yquant::verification::RunFundsDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--positions") {
    return yquant::verification::RunPositionsDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--reserve") {
    return yquant::verification::RunReservationDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--startup") {
    return yquant::verification::RunStartupDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--positions-lifecycle") {
    return yquant::verification::RunPositionHoldsDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--funds-lifecycle") {
    return yquant::verification::RunFundsLifecycleDemo();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::printf("Usage: yquant_scenarios "
                "[--maker|--orders|--cancel|--fills|--funds|"
                "--positions|--reserve|--startup|"
                "--funds-lifecycle|--positions-lifecycle]\n");
    return 0;
  }
  if (argc != 1) {
    std::fprintf(stderr, "Usage: yquant_scenarios "
                         "[--maker|--orders|--cancel|--fills|--funds|"
                         "--positions|--reserve|--startup|"
                         "--funds-lifecycle|--positions-lifecycle]\n");
    return 2;
  }
  constexpr std::uint64_t kMarketServiceId = 1;
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
  yquant::market::MarketServiceRegistry local_services;
  if (!local_services.Register(kMarketServiceId, market)) {
    std::fprintf(stderr, "Failed to register market service %" PRIu64 "\n",
                 kMarketServiceId);
    return 1;
  }
  yquant::market::MarketClientFactory factory(local_services);
  yquant::examples::QuoteStrategy strategy(output);
  auto client = factory.Create(kMarketServiceId, strategy);
  if (!client) {
    std::fprintf(stderr,
                 "Failed to create client for market service %" PRIu64 "\n",
                 kMarketServiceId);
    return 1;
  }
  client->Subscribe(kStrategyInstrumentId);
  yquant::container::Container container;
  const bool completed =
      container.Register(market) &&
      container.Run([&source] { return source.exhausted(); });
  client->Unsubscribe(kStrategyInstrumentId);
  if (!completed) {
    std::fprintf(stderr, "Failed to run the market service\n");
    return 1;
  }
  return 0;
}
