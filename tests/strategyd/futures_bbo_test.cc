#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/simulated_futures_exchange.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"

namespace yquant::strategyd {
namespace {

using loop_testing::Clock;
using loop_testing::kMs;
using loop_testing::StderrCapture;
using model::OrderSide;
using model::PositionEffect;

std::string FuturesMarketJson() {
  return R"({"adapter":"mock","source_id":1,"interval_ms":1,"instruments":[)"
         R"({"venue":"SHFE","symbol":"rb2701","price_decimals":0,"instrument_type":"futures",)"
         R"("price_tick":1,"quantity_step":1,"contract_multiplier":10,"margin_rate_ppm":100000,)"
         R"("open_fee_per_lot":0,"close_today_fee_per_lot":0,"close_yesterday_fee_per_lot":0,)"
         R"("bid_price":3500,"bid_qty":1,"ask_price":3501,"ask_qty":1,"price_step":1}]})";
}
std::string FuturesStrategyJson(std::int64_t quantity = 1,
                                std::int64_t limit = 1, std::int64_t gross = 2,
                                std::uint64_t timeout_ms = 100) {
  return R"({"strategy_id":"futures-test","parameters":{"source_id":1,"venue":"SHFE","symbol":"rb2701",)"
         R"("quote_qty":)" +
         std::to_string(quantity) + R"(,"min_position":)" +
         std::to_string(-limit) + R"(,"max_position":)" +
         std::to_string(limit) + R"(,"max_gross_position":)" +
         std::to_string(gross) + R"(,"quote_timeout_ms":)" +
         std::to_string(timeout_ms) + R"(,"retry_delay_ms":5}})";
}
std::string FuturesTradingJson(std::int64_t cash = 100000000000,
                               std::string_view positions = "[]") {
  return R"({"adapter":"simulated_futures","report_interval_ms":1,"shutdown_timeout_ms":50,"initial_cash":)" +
         std::to_string(cash) + R"(,"initial_futures_positions":)" +
         std::string(positions) + '}';
}

struct FuturesFrame {
  std::uint64_t at;
  std::int64_t bid;
  std::int64_t ask;
  std::int64_t quantity = 1;
};

class FuturesSource final : public MarketDataSource {
 public:
  std::array<FuturesFrame, 16> frames = {};
  std::size_t count = 0;
  base::Status Prepare(const MarketDataConfig& config) noexcept override {
    id_ = config.instruments[0].instrument.id;
    return {};
  }
  base::Status Start(std::span<const model::InstrumentId>,
                     std::uint64_t) noexcept override {
    active_ = true;
    return {};
  }
  MarketDataPollResult Poll(std::uint64_t now,
                            model::QuoteTick& out) noexcept override {
    if (!active_ || next_ == count || frames[next_].at > now) return {};
    const auto frame = frames[next_++];
    out = {};
    out.source_id = {1};
    out.instrument_id = id_;
    out.sequence = next_;
    out.receive_time_ns = now;
    out.bid_price = frame.bid;
    out.ask_price = frame.ask;
    out.bid_qty = frame.quantity;
    out.ask_qty = frame.quantity;
    out.flags = 7;
    return {MarketDataPollState::kEvent, {}};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return active_ && next_ < count
               ? std::optional<std::uint64_t>{frames[next_].at}
               : std::nullopt;
  }
  base::Status Stop() noexcept override {
    active_ = false;
    return {};
  }
  const char* error_detail() const noexcept override {
    return "scripted futures source";
  }

 private:
  model::InstrumentId id_;
  std::size_t next_ = 0;
  bool active_ = false;
};

// Observes the actual exchange boundary, without replacing its matching or
// normalized reports. Inventory counters select a deterministic stop point.
class TrackingExchange final : public TradingAdapter {
 public:
  std::array<model::OrderRequest, 64> requests = {};
  std::array<TradingEvent, 64> events = {};
  std::array<std::uint64_t, 64> cancel_times = {};
  std::size_t sent = 0;
  std::size_t reported = 0;
  std::size_t canceled = 0;
  std::int64_t longs = 0;
  std::int64_t shorts = 0;
  std::int64_t max_gross = 0;
  bool had_inventory = false;
  bool locked = false;
  base::Status Prepare(const TradingConfig& config) noexcept override {
    for (std::size_t index = 0;
         index < config.initial_account.futures_position_count; ++index) {
      const auto& p = config.initial_account.futures_positions[index];
      longs += p.long_today.quantity + p.long_yesterday.quantity;
      shorts += p.short_today.quantity + p.short_yesterday.quantity;
    }
    had_inventory = longs + shorts != 0;
    max_gross = longs + shorts;
    return exchange_.Prepare(config);
  }
  base::Status Start(std::uint64_t now) noexcept override {
    return exchange_.Start(now);
  }
  base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override {
    return exchange_.ReadAccountSnapshot(out);
  }
  base::Status SendOrder(model::ClientOrderId id,
                         const model::OrderRequest& request,
                         std::uint64_t now) noexcept override {
    if (sent == requests.size())
      return {base::ErrorCode::kResourceExhausted, {}};
    requests[sent++] = request;
    return exchange_.SendOrder(id, request, now);
  }
  base::Status SendCancel(model::ClientOrderId id,
                          const model::BrokerOrderId& broker,
                          model::CancelRequestId cancel,
                          std::uint64_t now) noexcept override {
    if (canceled == cancel_times.size())
      return {base::ErrorCode::kResourceExhausted, {}};
    cancel_times[canceled++] = now;
    return exchange_.SendCancel(id, broker, cancel, now);
  }
  base::Status OnQuote(const model::QuoteTick& quote,
                       std::uint64_t now) noexcept override {
    return exchange_.OnQuote(quote, now);
  }
  TradingPollResult Poll(std::uint64_t now,
                         TradingEvent& out) noexcept override {
    const auto result = exchange_.Poll(now, out);
    if (result.state == TradingPollState::kEvent) {
      if (reported == events.size())
        return {TradingPollState::kError,
                {base::ErrorCode::kResourceExhausted, {}}};
      events[reported++] = out;
      if (out.kind == model::OrderUpdateKind::kFill) {
        const auto qty = out.fill.quantity;
        if (out.fill.position_effect == PositionEffect::kOpen) {
          if (out.fill.side == OrderSide::kBuy)
            longs += qty;
          else
            shorts += qty;
        } else if (out.fill.side == OrderSide::kBuy)
          shorts -= qty;
        else
          longs -= qty;
        had_inventory = had_inventory || longs + shorts != 0;
        if (longs + shorts > max_gross) max_gross = longs + shorts;
        locked = locked || (longs != 0 && shorts != 0);
      }
    }
    return result;
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return exchange_.next_deadline();
  }
  bool is_drained() const noexcept override { return exchange_.is_drained(); }
  base::Status Stop() noexcept override { return exchange_.Stop(); }
  const char* error_detail() const noexcept override {
    return exchange_.error_detail();
  }

 private:
  SimulatedFuturesExchange exchange_;
};

class FuturesEnvironment final : public EventLoopEnvironment {
 public:
  FuturesEnvironment(Clock& clock, const TrackingExchange& exchange)
      : clock_(clock), exchange_(exchange) {}
  std::uint64_t stop_at = 20 * kMs;
  bool stop_when_flat = true;
  int checks = 0;
  int waits = 0;
  base::Status CheckStop(bool& out) noexcept override {
    if (++checks > 1000) return {base::ErrorCode::kInternalError, {}};
    out = clock_.now >= stop_at || (stop_when_flat && exchange_.had_inventory &&
                                    exchange_.longs + exchange_.shorts == 0);
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept override {
    if (clock_.now < stop_at && (!timeout || *timeout > stop_at - clock_.now))
      timeout = stop_at - clock_.now;
    if (!timeout || ++waits > 100) return {base::ErrorCode::kInternalError, {}};
    clock_.now += *timeout;
    return {};
  }

 private:
  Clock& clock_;
  const TrackingExchange& exchange_;
};

std::optional<std::int64_t> FuturesSummary(std::string_view text,
                                           std::string_view key) {
  const auto marker = text.rfind("futures_bbo: stopped ");
  if (marker == std::string_view::npos) return {};
  text.remove_prefix(marker);
  const auto found = text.find(key);
  if (found == std::string_view::npos) return {};
  text.remove_prefix(found + key.size());
  std::int64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} ? std::optional<std::int64_t>{value}
                                  : std::nullopt;
}

class FuturesScenario {
 public:
  FuturesScenario() : environment(clock, exchange) {}
  base::Status Run(std::string strategy_json = FuturesStrategyJson(),
                   std::int64_t cash = 100000000000,
                   std::string_view positions = "[]") {
    testing::ConfigFile market(FuturesMarketJson());
    testing::ConfigFile strategy(strategy_json);
    testing::ConfigFile trading(FuturesTradingJson(cash, positions));
    if (!market.valid() || !strategy.valid() || !trading.valid())
      return {base::ErrorCode::kIoError, {}};
    StrategyRunner runner(&clock, &source, nullptr, &exchange);
    auto result = runner.Handle(LoadAndStart{YQUANT_TEST_FUTURES_BBO_PATH,
                                             strategy.path(), market.path(),
                                             trading.path()});
    if (result.ok()) result = runner.Run(environment);
    detail = runner.error_detail();
    output = capture.Read();
    return result;
  }
  Clock clock;
  FuturesSource source;
  TrackingExchange exchange;
  FuturesEnvironment environment;
  StderrCapture capture;
  std::string detail;
  std::string output;
};

class FuturesDirectionTest : public ::testing::TestWithParam<bool> {};
TEST_P(FuturesDirectionTest, OpensFromFlatThenClosesTodayBackToFlat) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  const bool short_first = GetParam();
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.frames[1] = {2 * kMs, short_first ? 3501 : 3499,
                               short_first ? 3502 : 3500};
  scenario.source.frames[2] = {2500000, 3500, 3501};
  scenario.source.frames[3] = {6 * kMs, short_first ? 3499 : 3501,
                               short_first ? 3500 : 3502};
  scenario.source.count = 4;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.detail;
  std::array<TradingEvent, 2> fills;
  std::size_t count = 0;
  for (std::size_t index = 0; index < scenario.exchange.reported; ++index) {
    if (scenario.exchange.events[index].kind == model::OrderUpdateKind::kFill &&
        count < fills.size())
      fills[count++] = scenario.exchange.events[index];
  }
  ASSERT_EQ(count, 2U);
  EXPECT_EQ(fills[0].fill.side,
            short_first ? OrderSide::kSell : OrderSide::kBuy);
  EXPECT_EQ(fills[0].fill.position_effect, PositionEffect::kOpen);
  EXPECT_EQ(fills[1].fill.side,
            short_first ? OrderSide::kBuy : OrderSide::kSell);
  EXPECT_EQ(fills[1].fill.position_effect, PositionEffect::kCloseToday);
  EXPECT_EQ(FuturesSummary(scenario.output, "net="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "gross="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "margin="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "reserved="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "frozen_qty="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "realized_pnl="), 10000000);
  EXPECT_NE(scenario.output.find("futures_bbo: round trip completed\n"),
            std::string::npos);
}
INSTANTIATE_TEST_SUITE_P(FuturesDirections, FuturesDirectionTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "Short" : "Long";
                         });

TEST(FuturesBboTest,
     OpposingOpenRaceIsClosedExplicitlyRatherThanMistakingNetZeroForFlat) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.frames[1] = {2 * kMs, 3499, 3500};
  scenario.source.frames[2] = {2 * kMs, 3501, 3502};
  scenario.source.frames[3] = {4 * kMs, 3500, 3501};
  scenario.source.frames[4] = {8 * kMs, 3501, 3502};
  scenario.source.count = 5;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.detail;
  EXPECT_TRUE(scenario.exchange.locked);
  EXPECT_EQ(scenario.exchange.max_gross, 2);
  std::size_t opens = 0;
  std::size_t closes = 0;
  for (std::size_t index = 0; index < scenario.exchange.sent; ++index) {
    if (scenario.exchange.requests[index].position_effect ==
        PositionEffect::kOpen)
      ++opens;
    else
      ++closes;
  }
  EXPECT_EQ(opens, 2U);
  EXPECT_GE(closes, 2U);
  EXPECT_EQ(FuturesSummary(scenario.output, "gross="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "max_gross_seen="), 2);
  EXPECT_EQ(FuturesSummary(scenario.output, "frozen_qty="), 0);
  EXPECT_NE(scenario.output.find("futures_bbo: locked inventory observed\n"),
            std::string::npos);
}

TEST(FuturesBboTest, PartialFillCancelReplacesOpenWithOnlyTheClosableQuantity) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.environment.stop_when_flat = false;
  scenario.environment.stop_at = 4 * kMs;
  scenario.source.frames[0] = {0, 3500, 3501, 1};
  scenario.source.frames[1] = {2 * kMs, 3499, 3500, 1};
  scenario.source.count = 2;
  ASSERT_TRUE(scenario.Run(FuturesStrategyJson(2, 2, 4)).ok())
      << scenario.detail;
  bool close_one = false;
  for (std::size_t index = 0; index < scenario.exchange.sent; ++index) {
    const auto& request = scenario.exchange.requests[index];
    if (request.side == OrderSide::kSell &&
        request.position_effect == PositionEffect::kCloseToday &&
        request.quantity == 1)
      close_one = true;
  }
  EXPECT_TRUE(close_one);
  EXPECT_EQ(FuturesSummary(scenario.output, "net="), 1);
  EXPECT_EQ(FuturesSummary(scenario.output, "gross="), 1);
  EXPECT_EQ(FuturesSummary(scenario.output, "frozen_qty="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "reserved="), 0);
}

TEST(FuturesBboTest, NoFurtherQuotesStillExpireAndCancelBothSidesOnTheTimer) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.environment.stop_at = 8 * kMs;
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.count = 1;
  ASSERT_TRUE(scenario.Run(FuturesStrategyJson(1, 1, 2, 5)).ok())
      << scenario.detail;
  ASSERT_EQ(scenario.exchange.canceled, 2U);
  EXPECT_EQ(scenario.exchange.cancel_times[0], 5 * kMs);
  EXPECT_EQ(scenario.exchange.cancel_times[1], 5 * kMs);
  EXPECT_EQ(FuturesSummary(scenario.output, "reserved="), 0);
  EXPECT_EQ(FuturesSummary(scenario.output, "slots="), 0);
}

TEST(FuturesBboTest, MarginFailureUsesBackoffWithoutSendingUnfundedOrders) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.environment.stop_at = 12 * kMs;
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.count = 1;
  ASSERT_TRUE(scenario.Run(FuturesStrategyJson(), 0).ok()) << scenario.detail;
  EXPECT_EQ(scenario.exchange.sent, 0U);
  EXPECT_EQ(FuturesSummary(scenario.output, "submitted="), 0);
  EXPECT_EQ(scenario.environment.waits, 3);
  EXPECT_LT(scenario.environment.checks, 20);
}

TEST(FuturesBboTest, GrossLimitCountsBothPendingOpenOrdersIndependently) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.environment.stop_at = 3 * kMs;
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.count = 1;
  ASSERT_TRUE(scenario.Run(FuturesStrategyJson(1, 1, 1)).ok())
      << scenario.detail;
  ASSERT_EQ(scenario.exchange.sent, 1U);
  EXPECT_EQ(scenario.exchange.requests[0].side, OrderSide::kBuy);
  EXPECT_EQ(scenario.exchange.requests[0].position_effect,
            PositionEffect::kOpen);
  EXPECT_EQ(FuturesSummary(scenario.output, "gross="), 0);
}

TEST(FuturesBboTest, YesterdayInventorySelectsCloseYesterdayBeforeOpening) {
  FuturesScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 3500, 3501};
  scenario.source.frames[1] = {2 * kMs, 3499, 3500};
  scenario.source.count = 2;
  constexpr std::string_view kPosition =
      R"([{"venue":"SHFE","symbol":"rb2701","reference_price":3501,)"
      R"("long_today":{"quantity":0,"open_price":0},"long_yesterday":{"quantity":0,"open_price":0},)"
      R"("short_today":{"quantity":0,"open_price":0},"short_yesterday":{"quantity":1,"open_price":3501}}])";
  ASSERT_TRUE(scenario.Run(FuturesStrategyJson(), 100000000000, kPosition).ok())
      << scenario.detail;
  ASSERT_GE(scenario.exchange.sent, 1U);
  EXPECT_EQ(scenario.exchange.requests[0].side, OrderSide::kBuy);
  EXPECT_EQ(scenario.exchange.requests[0].position_effect,
            PositionEffect::kCloseYesterday);
  EXPECT_EQ(FuturesSummary(scenario.output, "gross="), 0);
}

}  // namespace
}  // namespace yquant::strategyd
