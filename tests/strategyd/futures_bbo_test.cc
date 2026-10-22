#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"
#include "yquant/trading/simulated_futures_exchange.h"

namespace yquant::strategyd {
namespace {

using loop_testing::Clock;
using loop_testing::kMs;
using loop_testing::StderrCapture;
using model::OrderSide;
using model::PositionEffect;

std::string FuturesMarketJson(bool generic = false) {
  const std::string selected = generic
                                   ? R"({"venue":"DCE","symbol":"a2701"})"
                                   : R"({"venue":"SHFE","symbol":"rb2701"})";
  return R"({"adapter":"shm","source_id":1,"runtime_directory":"/tmp/yquant","stream":"futures","instrument_catalog":")" +
         std::string(YQUANT_TEST_INSTRUMENT_CATALOG_PATH) +
         R"(","instruments":[)" + selected + "]}";
}
std::string FuturesStrategyJson(std::int64_t quantity = 1,
                                std::int64_t limit = 1, std::int64_t gross = 2,
                                std::uint64_t timeout_ms = 100,
                                bool generic = false) {
  const std::string selected = generic ? R"("venue":"DCE","symbol":"a2701",)"
                                       : R"("venue":"SHFE","symbol":"rb2701",)";
  return R"({"strategy_id":"futures-test","parameters":{)" + selected +
         R"("quote_qty":)" + std::to_string(quantity) + R"(,"min_position":)" +
         std::to_string(-limit) + R"(,"max_position":)" +
         std::to_string(limit) + R"(,"max_gross_position":)" +
         std::to_string(gross) + R"(,"quote_timeout_ms":)" +
         std::to_string(timeout_ms) + R"(,"retry_delay_ms":5}})";
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
  base::Status Prepare(const md::MarketDataConfig& config) noexcept override {
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

// Runs the futures plugin against scripted gateway reports and account state.
// Negative available cash blocks opening while retaining the close-only path.
void CheckFuturesServiceScenario(bool closing, bool insufficient_cash = false,
                                 bool generic = false) {
  testing::ConfigFile generic_catalog(
      R"({"instruments":[{"venue":"DCE","symbol":"a2701",)"
      R"("instrument_type":"futures","price_decimals":0,"price_tick":1,)"
      R"("quantity_step":1,"contract_multiplier":10,"margin_rate_ppm":100000,)"
      R"("open_fee_per_lot":2000000,"close_today_fee_per_lot":2000000,)"
      R"("close_yesterday_fee_per_lot":2000000}]})");
  auto market_json = FuturesMarketJson(generic);
  if (generic) {
    const std::string path = YQUANT_TEST_INSTRUMENT_CATALOG_PATH;
    market_json.replace(market_json.find(path), path.size(),
                        generic_catalog.path());
  }
  testing::ConfigFile market(market_json);
  testing::ConfigFile strategy(insufficient_cash || generic
                                   ? FuturesStrategyJson(1, 2, 3, 100, generic)
                                   : FuturesStrategyJson());
  ASSERT_TRUE(market.valid());
  ASSERT_TRUE(strategy.valid());
  Clock clock;
  FuturesSource source;
  source.frames[0] = {0, 3500, 3501};
  source.frames[1] = {kMs, 3500, 3501};
  source.count = 2;
  loop_testing::Environment environment(clock);
  environment.stop_at = 10 * kMs;
  testing::MockTradingService service;
  service.broker_managed = generic;
  auto& account = service.initial_account;
  account.type = model::AccountType::kFutures;
  account.futures.balance = account.futures.equity = account.futures.available =
      100000000000;
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId(generic ? "DCE" : "SHFE",
                                    generic ? "a2701" : "rb2701", id),
            base::ErrorCode::kOk);
  if (closing) {
    account.futures_position_count = 1;
    account.futures_positions[0].instrument_id = id;
    account.futures_positions[0].long_today = {1, 0, 35000000000};
    if (generic) {
      account.futures_positions[0].long_yesterday = {1, 0, 34900000000};
      account.futures_positions[0].unallocated_long_frozen = 1;
    }
  }
  if (insufficient_cash) {
    // One lot at 3500 with a 10x multiplier and 10% margin needs 3500 yuan.
    // The net/gross bounds permit a new buy; only negative available cash
    // blocks opening while still allowing the existing long to be closed.
    account.futures.balance = account.futures.equity = 3499999999;
    account.futures.margin = 3500000000;
    account.futures.available = -1;
    account.futures_positions[0].margin = 3500000000;
  }
  for (std::uint64_t i = 1; i <= (closing ? 1U : 2U); ++i) {
    testing::MockTradingService::Event accepted;
    accepted.deadline = closing ? 5 * kMs : kMs;
    auto& d = accepted.dispatch;
    d.notify_order = true;
    d.update.kind = model::OrderUpdateKind::kAccepted;
    auto& order = d.update.order;
    order.client_order_id = {i};
    order.instrument_id = id;
    order.side = closing ? OrderSide::kSell
                         : (i == 1 ? OrderSide::kBuy : OrderSide::kSell);
    order.position_effect = closing ? (generic ? PositionEffect::kClose
                                               : PositionEffect::kCloseToday)
                                    : PositionEffect::kOpen;
    order.limit_price = closing ? 3501 : (i == 1 ? 3500 : 3501);
    order.original_qty = order.leaves_qty = 1;
    order.state = model::OrderState::kWorking;
    if (closing) {
      accepted.update_account = true;
      accepted.account = account;
      accepted.account.futures_positions[0].long_today.frozen_qty = 1;
    }
    service.events.push_back(accepted);
    auto canceled = accepted;
    canceled.deadline = 10 * kMs;
    canceled.dispatch.update.kind = model::OrderUpdateKind::kCanceled;
    canceled.dispatch.update.order.state = model::OrderState::kCanceled;
    canceled.dispatch.update.order.leaves_qty = 0;
    if (closing) canceled.account = account;
    service.stopping_events.push_back(canceled);
  }
  StderrCapture capture;
  ASSERT_TRUE(capture.valid());
  RunnerTestHarness runner(clock, environment, source, service);
  ASSERT_TRUE(runner
                  .LoadAndStart({YQUANT_TEST_FUTURES_BBO_PATH, strategy.path(),
                                 market.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  ASSERT_EQ(service.requests.size(), closing ? 1U : 2U);
  EXPECT_EQ(
      service.requests[0].position_effect,
      closing ? (generic ? PositionEffect::kClose : PositionEffect::kCloseToday)
              : PositionEffect::kOpen);
  EXPECT_EQ(service.requests[0].side,
            closing ? OrderSide::kSell : OrderSide::kBuy);
  if (!closing) {
    EXPECT_EQ(service.requests[1].position_effect, PositionEffect::kOpen);
    EXPECT_EQ(service.requests[1].side, OrderSide::kSell);
  }
  const auto output = capture.Read();
  EXPECT_NE(output.find("slots=0"), std::string::npos);
  EXPECT_NE(output.find("futures_bbo: first quotes submitted\n"),
            std::string::npos);
  const auto both_sides = output.find("futures_bbo: both sides submitted\n");
  if (closing) {
    EXPECT_EQ(both_sides, std::string::npos);
  } else {
    ASSERT_NE(both_sides, std::string::npos);
    EXPECT_EQ(
        output.find("futures_bbo: both sides submitted\n", both_sides + 1),
        std::string::npos);
  }
}
TEST(FuturesBboServiceTest,
     OpensBothDirectionsAndDrainsScriptedGatewayReports) {
  CheckFuturesServiceScenario(false);
}
TEST(FuturesBboServiceTest, PendingCloseWaitsForGatewayFrozenPosition) {
  CheckFuturesServiceScenario(true);
}
TEST(FuturesBboServiceTest,
     InsufficientOpeningCashAllowsOnlyCloseWithoutBothSidesMarker) {
  CheckFuturesServiceScenario(/*closing=*/true, /*insufficient_cash=*/true);
}
TEST(FuturesBboServiceTest, BrokerGenericClosePreservesUndistributedFreeze) {
  CheckFuturesServiceScenario(/*closing=*/true, /*insufficient_cash=*/false,
                              /*generic=*/true);
}

TEST(FuturesBboServiceTest, BrokerAccountLeadingClosingReportsDoesNotReopen) {
  testing::ConfigFile catalog(
      R"({"instruments":[{"venue":"DCE","symbol":"a2701",)"
      R"("instrument_type":"futures","price_decimals":0,"price_tick":1,)"
      R"("quantity_step":1,"contract_multiplier":10,"margin_rate_ppm":100000,)"
      R"("open_fee_per_lot":2000000,"close_today_fee_per_lot":2000000,)"
      R"("close_yesterday_fee_per_lot":2000000}]})");
  auto market_json = FuturesMarketJson(true);
  const std::string original = YQUANT_TEST_INSTRUMENT_CATALOG_PATH;
  market_json.replace(market_json.find(original), original.size(),
                      catalog.path());
  testing::ConfigFile market(market_json);
  testing::ConfigFile strategy(FuturesStrategyJson(1, 1, 2, 2, true));
  Clock clock;
  FuturesSource source;
  source.frames[0] = {0, 3500, 3501};
  source.count = 1;
  loop_testing::Environment environment(clock);
  environment.stop_at = 4 * kMs;
  testing::MockTradingService service;
  service.broker_managed = true;
  auto& initial = service.initial_account;
  initial.type = model::AccountType::kFutures;
  initial.futures.balance = initial.futures.equity = initial.futures.available =
      100000000000;
  initial.futures_position_count = 1;
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("DCE", "a2701", id), base::ErrorCode::kOk);
  initial.futures_positions[0].instrument_id = id;
  initial.futures_positions[0].long_today = {1, 0, 35000000000};
  testing::MockTradingService::Event accepted;
  accepted.deadline = kMs;
  accepted.update_account = true;
  accepted.account = initial;
  // The broker query covers the closing trade already, while its per-order
  // stream still has Accepted followed by a later unique Fill to dispatch.
  accepted.account.futures_positions[0].long_today = {};
  auto& update = accepted.dispatch.update;
  accepted.dispatch.notify_order = true;
  update.kind = model::OrderUpdateKind::kAccepted;
  update.order.client_order_id = {1};
  ASSERT_EQ(update.order.broker_order_id.Assign("broker-close"),
            base::ErrorCode::kOk);
  update.order.instrument_id = id;
  update.order.side = OrderSide::kSell;
  update.order.position_effect = PositionEffect::kClose;
  update.order.original_qty = update.order.leaves_qty = 1;
  update.order.limit_price = 3501;
  update.order.state = model::OrderState::kWorking;
  update.order.report_sequence = 1;
  service.events.push_back(accepted);
  auto filled = accepted;
  filled.deadline = 3 * kMs;
  filled.dispatch.update.kind = model::OrderUpdateKind::kFill;
  filled.dispatch.update.order.state = model::OrderState::kFilled;
  filled.dispatch.update.order.cumulative_filled_qty = 1;
  filled.dispatch.update.order.leaves_qty = 0;
  filled.dispatch.update.order.report_sequence = 2;
  filled.dispatch.has_fill = true;
  auto& fill = filled.dispatch.fill;
  fill.client_order_id = {1};
  fill.broker_order_id = update.order.broker_order_id;
  ASSERT_EQ(fill.execution_id.Assign("close-trade"), base::ErrorCode::kOk);
  fill.instrument_id = id;
  fill.side = OrderSide::kSell;
  fill.position_effect = PositionEffect::kClose;
  fill.price = 3501;
  fill.quantity = 1;
  service.events.push_back(filled);
  StderrCapture capture;
  ASSERT_TRUE(capture.valid());
  RunnerTestHarness runner(clock, environment, source, service);
  ASSERT_TRUE(runner
                  .LoadAndStart({YQUANT_TEST_FUTURES_BBO_PATH, strategy.path(),
                           market.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  ASSERT_EQ(service.requests.size(), 1);
  EXPECT_EQ(service.requests[0].position_effect, PositionEffect::kClose);
  EXPECT_EQ(service.cancel_calls, 0);
  EXPECT_EQ(service.delivered, 2);
  EXPECT_NE(capture.Read().find("fills=1 filled_qty=1 net=0 gross=0"),
            std::string::npos);
}
}  // namespace
}  // namespace yquant::strategyd
