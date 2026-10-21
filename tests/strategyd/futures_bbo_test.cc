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

std::string FuturesMarketJson() {
  return R"({"adapter":"shm","source_id":1,"runtime_directory":"/tmp/yquant","stream":"futures","instrument_catalog":")" +
         std::string(YQUANT_TEST_INSTRUMENT_CATALOG_PATH) +
         R"(","instruments":[{"venue":"SHFE","symbol":"rb2701"}]})";
}
std::string FuturesStrategyJson(std::int64_t quantity = 1,
                                std::int64_t limit = 1, std::int64_t gross = 2,
                                std::uint64_t timeout_ms = 100) {
  return R"({"strategy_id":"futures-test","parameters":{"venue":"SHFE","symbol":"rb2701",)"
         R"("quote_qty":)" +
         std::to_string(quantity) + R"(,"min_position":)" +
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

// Observes the actual exchange boundary, without replacing its matching or
// normalized reports. Inventory counters select a deterministic stop point.

void CheckFuturesServiceScenario(bool closing) {
  testing::ConfigFile market(FuturesMarketJson());
  testing::ConfigFile strategy(FuturesStrategyJson());
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
  auto& account = service.initial_account;
  account.type = model::AccountType::kFutures;
  account.futures.balance = account.futures.equity = account.futures.available =
      100000000000;
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", id),
            base::ErrorCode::kOk);
  if (closing) {
    account.futures_position_count = 1;
    account.futures_positions[0].instrument_id = id;
    account.futures_positions[0].long_today = {1, 0, 35000000000};
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
    order.position_effect =
        closing ? PositionEffect::kCloseToday : PositionEffect::kOpen;
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
                  .Handle({YQUANT_TEST_FUTURES_BBO_PATH, strategy.path(),
                           market.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  ASSERT_EQ(service.requests.size(), closing ? 1U : 2U);
  EXPECT_EQ(service.requests[0].position_effect,
            closing ? PositionEffect::kCloseToday : PositionEffect::kOpen);
  EXPECT_EQ(service.requests[0].side,
            closing ? OrderSide::kSell : OrderSide::kBuy);
  if (!closing) {
    EXPECT_EQ(service.requests[1].position_effect, PositionEffect::kOpen);
    EXPECT_EQ(service.requests[1].side, OrderSide::kSell);
  }
  EXPECT_NE(capture.Read().find("slots=0"), std::string::npos);
}
TEST(FuturesBboServiceTest,
     OpensBothDirectionsAndDrainsScriptedGatewayReports) {
  CheckFuturesServiceScenario(false);
}
TEST(FuturesBboServiceTest, PendingCloseWaitsForGatewayFrozenPosition) {
  CheckFuturesServiceScenario(true);
}
}  // namespace
}  // namespace yquant::strategyd
