#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>

#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/market/simnow_quote_source.h"

namespace {
std::string_view Env(const char* name) {
  const auto* value = std::getenv(name);
  return value ? value : "";
}

// Checks consecutive per-contract sequences at the real client callback.
class Capture final : public yquant::market::QuoteListener {
 public:
  std::map<std::uint32_t, std::pair<std::string, std::uint64_t>> contracts;
  bool valid = true;
  void OnQuote(const yquant::model::Quote& quote) override {
    auto it = contracts.find(quote.instrument_id);
    if (it == contracts.end() || quote.sequence != it->second.second + 1) { valid = false; return; }
    if (!it->second.second) {
      std::printf("multi_quote symbol=%s instrument=%u bid=%.17g ask=%.17g\n",
                  it->second.first.c_str(), quote.instrument_id, quote.bid_price, quote.ask_price);
      std::fflush(stdout);
    }
    it->second.second = quote.sequence;
  }
  bool complete() const {
    for (const auto& [id, row] : contracts) if (!row.second) return false;
    return true;
  }
};
}  // namespace

int main(int argc, char** argv) {
  const bool anonymous = argc > 1 && std::string_view(argv[1]) == "--anonymous";
  if (anonymous) { --argc; ++argv; }
  if (argc < 7 || argc % 2 != 1) {
    std::fputs("Usage: verify_simnow_multi_market [--anonymous] FRONT SECONDS SYMBOL ID SYMBOL ID ...\n",stderr);
    return 2;
  }
  int seconds = 0;
  const std::string_view timeout = argv[2];
  auto [end, ec] = std::from_chars(timeout.data(),timeout.data()+timeout.size(),seconds);
  if (ec != std::errc{} || end != timeout.data()+timeout.size() || seconds < 1 || seconds > 3600) return 2;
  yquant::market::SimNowMarketConfig config;
  config.front = argv[1];
  config.broker = Env("YQUANT_CTP_BROKER_ID");
  config.user = Env("YQUANT_CTP_USER_ID");
  config.password = Env("YQUANT_CTP_PASSWORD");
  config.anonymous = anonymous;
  Capture capture;
  for (int i=3;i<argc;i+=2) {
    std::uint32_t id = 0;
    const std::string_view text = argv[i+1];
    auto [ptr, code] = std::from_chars(text.data(),text.data()+text.size(),id);
    if (code != std::errc{} || ptr != text.data()+text.size() || !id) return 2;
    config.subscriptions.push_back({argv[i],id});
    capture.contracts.emplace(id,std::pair{argv[i],0});
  }
  auto source = yquant::market::SimNowQuoteSource::Connect(config);
  if (!source) { std::fputs("Invalid multi-market configuration\n",stderr); return 2; }
  yquant::market::MarketService market(*source);
  yquant::market::MarketServiceRegistry registry;
  registry.Register(1,market);
  auto client = yquant::market::MarketClientFactory(registry).Create(1,capture);
  for (const auto& sub : config.subscriptions) client->Subscribe(sub.instrument_id);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  while (!source->failed() && !capture.complete() && capture.valid &&
         std::chrono::steady_clock::now() < deadline) {
    market.Run();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (const auto& sub : config.subscriptions) client->Unsubscribe(sub.instrument_id);
  if (!source->ready() || !capture.complete() || !capture.valid) {
    std::fputs("Multi-market subscription failed or timed out\n",stderr);
    return 1;
  }
  std::printf("simnow_multi_verified contracts=%zu\n",config.subscriptions.size());
  return 0;
}
