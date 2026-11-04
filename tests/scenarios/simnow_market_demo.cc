#include "simnow_market_demo.h"

#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "strategies/quote_strategy.h"
#include "yquant/container/container.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/market/simnow_quote_source.h"

namespace yquant::verification {
namespace {

std::string_view Env(const char* name) {
  const char* value = std::getenv(name);
  return value ? value : "";
}

bool Positive(std::string_view text, int limit, int& value) {
  const auto end = text.data() + text.size();
  auto [ptr, ec] = std::from_chars(text.data(), end, value);
  return ec == std::errc{} && ptr == end && value > 0 && value <= limit;
}

// Prints the quote as read on the business thread and the strategy's separate
// response. Stops reading after the requested count, even within one Run batch.
class Verification final : public market::QuoteSource,
                           public examples::QuoteIntentListener {
 public:
  Verification(market::QuoteSource& source, const char* symbol, int count)
      : source_(source), symbol_(symbol), limit_(count) {}

  // QuoteSource implementation.
  bool TryRead(model::Quote& quote) override {
    if (done()) return false;
    const auto result = source_.TryRead(quote);
    if (result) {
      std::printf("simnow_quote symbol=%s instrument=%" PRIu32
                  " sequence=%" PRIu64 " bid=%.17g ask=%.17g"
                  " bid_qty=%d ask_qty=%d\n",
                  symbol_, quote.instrument_id, quote.sequence, quote.bid_price,
                  quote.ask_price, quote.bid_quantity, quote.ask_quantity);
    }
    return result;
  }
  // QuoteIntentListener implementation.
  void OnQuoteIntent(const examples::QuoteIntent& intent) override {
    std::printf("quote_intent instrument=%" PRIu32 " sequence=%" PRIu64
                " bid=%.17g ask=%.17g\n", intent.instrument_id, intent.sequence,
                intent.bid_price, intent.ask_price);
    ++received_;
  }
  bool done() const { return received_ == limit_; }

 private:
  market::QuoteSource& source_;
  const char* symbol_;
  int limit_;
  int received_ = 0;
};

}  // namespace

int RunSimNowMarket(int argc, const char* const* argv,
                    decltype(&market::SimNowQuoteSource::Connect) connect) {
  const bool anonymous = argc > 0 && std::string_view(argv[0]) == "--anonymous";
  if (anonymous) {
    --argc;
    ++argv;
  }
  int count = 1, seconds = 30;
  if (argc < 2 || argc > 4 ||
      (argc >= 3 && !Positive(argv[2], 10000, count)) ||
      (argc == 4 && !Positive(argv[3], 3600, seconds))) {
    std::fprintf(stderr, "Usage: simnow_market_probe [--anonymous] FRONT SYMBOL "
                         "[COUNT [TIMEOUT_SECONDS]]\n");
    return 2;
  }
  constexpr std::uint32_t kInstrument = 1;
  const market::SimNowMarketConfig config{
      argv[0], argv[1], Env("YQUANT_CTP_BROKER_ID"), Env("YQUANT_CTP_USER_ID"),
      Env("YQUANT_CTP_PASSWORD"), kInstrument, anonymous};
  if (!anonymous && (config.broker.empty() || config.user.empty() ||
                     config.password.empty())) {
    std::fprintf(stderr, "Set YQUANT_CTP_BROKER_ID, YQUANT_CTP_USER_ID and "
                         "YQUANT_CTP_PASSWORD before connecting\n");
    return 2;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(seconds);
  auto source = connect(config);
  if (!source) {
    std::fprintf(stderr, "Invalid SimNow config or unavailable market API\n");
    return 1;
  }
  Verification output(*source, argv[1], count);
  market::MarketService market_service(output);
  market::MarketServiceRegistry registry;
  examples::QuoteStrategy strategy(output);
  auto client = registry.Register(1, market_service)
      ? market::MarketClientFactory(registry).Create(1, strategy) : nullptr;
  if (!client) {
    std::fprintf(stderr, "Failed to create SimNow market client\n");
    return 1;
  }
  client->Subscribe(kInstrument);
  container::Container container;
  const bool ran = container.Register(market_service) &&
      container.Run([&] {
        return output.done() || source->failed() ||
               std::chrono::steady_clock::now() >= deadline;
      });
  client->Unsubscribe(kInstrument);
  if (!ran) {
    std::fprintf(stderr, "Failed to run SimNow market service\n");
    return 1;
  }
  if (source->failed()) return 1;
  if (!output.done()) {
    std::fprintf(stderr, "SimNow market timeout after %d seconds\n", seconds);
    return 1;
  }
  std::printf("simnow_summary symbol=%s quotes=%d\n", argv[1], count);
  return 0;
}

}  // namespace yquant::verification
