#include "maker_demo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>

#include "yquant/container/container.h"
#include "yquant/market/market_client_factory.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"
#include "yquant/strategies/market_maker/market_maker.h"
#include "yquant/trading/simulated_trading_adapter.h"
#include "yquant/trading/trading_client_factory.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::verification {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kWarmup = 256;
constexpr std::size_t kSamples = 1024;
constexpr auto kQuotes = kWarmup + kSamples;

// Presents one quote per market-service turn. The second quote repeats the
// first; subsequent quotes change only the bid. The empty turn ends the run.
class MakerFeed final : public market::QuoteSource {
 public:
  // market::QuoteSource implementation.
  bool TryRead(model::Quote& quote) override {
    measuring = false;
    if (pause_) {
      pause_ = false;
      return false;
    }
    if (count == kQuotes) {
      done = true;
      return false;
    }
    const double bid = count >= 2 && count % 2 == 0 ? 101 : 100;
    quote = {42, bid, 102, 10, 10, count + 1};
    measuring = count >= kWarmup;
    ++count;
    pause_ = true;
    start = Clock::now();
    return true;
  }

  Clock::time_point start;
  std::size_t count = 0;
  bool measuring = false;
  bool done = false;

 private:
  bool pause_ = false;
};

// Measures the first request caused synchronously by a measured quote.
// Counter behavior and callbacks come from the normal simulated adapter.
class MeasuredCounter final : public trading::ITradingAdapter {
 public:
  explicit MeasuredCounter(MakerFeed& feed) : feed_(feed) {}

  // trading::ITradingAdapter implementation.
  void SetCallback(trading::ITradingAdapterCallback& callback) override {
    counter.SetCallback(callback);
  }
  bool StartAccountSync(std::uint64_t id) override {
    return counter.StartAccountSync(id);
  }
  trading::SendResult SubmitOrder(
      const trading::SubmitOrderRequest& r) override {
    Record(true);
    ++submits;
    const bool ask = submits == 2;
    const double bid = submits > 2 && submits % 2 != 0 ? 101 : 100;
    const double price = ask ? 102 : bid;
    const auto side = ask ? trading::Side::kSell : trading::Side::kBuy;
    valid &= r.order == trading::OrderInfo{
        {100, submits}, 7, 2, 42, side, trading::Offset::kOpen, price, 2};
    valid &= submits <= 2 ? feed_.count == 1
                          : feed_.count == submits && cancels == submits - 2;
    if (feed_.count <= 3) {
      std::printf("maker_submit id=%" PRIu64 " side=%s price=%g\n",
                  r.order.order_id.sequence,
                  r.order.side == trading::Side::kBuy ? "buy" : "sell",
                  r.order.price);
    }
    return counter.SubmitOrder(r);
  }
  trading::SendResult CancelOrder(
      const trading::AdapterCancelOrderRequest& r) override {
    Record(false);
    ++cancels;
    const auto old_id = cancels == 1 ? 1 : cancels + 1;
    valid &= feed_.count == cancels + 2 && submits == cancels + 1 &&
             r.order_id == trading::OrderId{100, old_id} &&
             r.service_cancel_id == cancels;
    if (feed_.count <= 3) {
      std::printf("maker_cancel id=%" PRIu64 "\n", r.order_id.sequence);
    }
    return counter.CancelOrder(r);
  }
  void Run() override { counter.Run(); }

  trading::SimulatedTradingAdapter counter;
  std::array<std::int64_t, kSamples> ns{};
  std::array<std::int64_t, kSamples> replace_ns{};
  std::size_t samples = 0, replacements = 0, submits = 0, cancels = 0;
  bool valid = true;

 private:
  // This fixture settles each revision before releasing the next quote.
  void Record(bool submit) {
    if (feed_.count <= kWarmup) return;
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - feed_.start).count();
    if (feed_.measuring) {
      feed_.measuring = false;
      if (samples < kSamples) ns[samples++] = elapsed;
      else valid = false;
    }
    if (submit) {
      if (replacements < kSamples) replace_ns[replacements++] = elapsed;
      else valid = false;
    }
  }
  MakerFeed& feed_;
};

bool VerifyMaker() {
  MakerFeed feed;
  MeasuredCounter counter(feed);
  market::MarketService market_service(feed);
  trading::TradingService trading_service;
  market::MarketServiceRegistry markets;
  trading::TradingServiceRegistry traders;
  if (!markets.Register(1, market_service) ||
      !traders.Register(3, trading_service) ||
      !trading_service.BindAccount(7, counter)) return false;
  trading::FundsTerms terms;
  terms.currency = model::Currency("CNY");
  terms.contract_multiplier = 1;
  terms.margin_rate = 0.1;
  terms.fees[0] = trading::FeeRate{1, 0};
  for (auto side : {trading::Side::kBuy, trading::Side::kSell}) {
    if (!trading_service.ConfigureFundsTerms(7, 42, side, terms)) return false;
  }
  counter.counter.SetInitialState({terms.currency, 1000, 1000, 0, 0}, {});
  trading_service.Run();
  auto trader = trading::TradingClientFactory(traders).Create(3);
  if (!trader || !trading_service.IsAccountReady(7)) return false;
  trading::OrderIdGenerator ids(100);
  strategies::market_maker::MarketMaker maker(*trader, ids, 7, 2, 42, 2);
  if (!trader->BindListener(maker)) return false;
  auto market = market::MarketClientFactory(markets).Create(1, maker);
  if (!market) return false;
  market->Subscribe(42);
  container::Container container;
  const bool ran = container.Register(market_service) &&
                   container.Register(trading_service) &&
                   container.Run([&feed] { return feed.done; });
  market->Unsubscribe(42);
  trading::OrderSnapshot bid, ask;
  if (!ran || !counter.valid || counter.samples != kSamples ||
      counter.replacements != kSamples || counter.submits != kQuotes ||
      counter.cancels != kQuotes - 2 ||
      !trader->QueryOrder({{100, kQuotes}, 7, 2}, bid) ||
      !trader->QueryOrder({{100, 2}, 7, 2}, ask) ||
      bid.state != trading::OrderState::kAccepted || bid.order.price != 100 ||
      ask.state != trading::OrderState::kAccepted || ask.order.price != 102) {
    return false;
  }
  std::printf("maker_summary quotes=%zu submits=%zu cancels=%zu bid=%g ask=%g\n",
              feed.count, counter.submits, counter.cancels,
              bid.order.price, ask.order.price);
  const auto print_latency = [](const char* phase, auto& values) {
    std::sort(values.begin(), values.end());
    const auto percentile = [&](std::size_t p) {
      return values[(kSamples * p + 99) / 100 - 1];
    };
    std::printf("maker_latency_ns phase=%s warmup=%zu samples=%zu p50=%" PRId64
                " p95=%" PRId64 " p99=%" PRId64 "\n", phase, kWarmup, kSamples,
                percentile(50), percentile(95), percentile(99));
  };
  print_latency("cancel", counter.ns);
  print_latency("replace", counter.replace_ns);
  return true;
}

}  // namespace

int RunMakerDemo() {
  if (VerifyMaker()) return 0;
  std::fprintf(stderr, "Maker scenario failed\n");
  return 1;
}

}  // namespace yquant::verification
