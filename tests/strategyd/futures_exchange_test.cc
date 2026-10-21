#include <cstdint>
#include <string_view>

#include "gtest/gtest.h"
#include "strategyd/simulated_futures_exchange.h"
#include "strategyd/trading_limits.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;
using model::OrderSide;
using model::OrderUpdateKind;
using model::PositionEffect;

constexpr std::uint64_t kInterval = 1000000;

TradingConfig ExchangeConfig() {
  TradingConfig config;
  config.adapter = TradingAdapterKind::kSimulatedFutures;
  config.initial_account.type = model::AccountType::kFutures;
  config.initial_account.cash_balance = 100000000000;
  return config;
}

model::InstrumentId FutureId() {
  model::InstrumentId id;
  EXPECT_EQ(model::MakeInstrumentId("SHFE", "rb2701", id), ErrorCode::kOk);
  return id;
}

model::QuoteTick ExchangeQuote(std::uint64_t sequence = 1,
                               std::int64_t bid = 3500, std::int64_t ask = 3501,
                               std::int64_t bid_qty = 2,
                               std::int64_t ask_qty = 3) {
  model::QuoteTick quote;
  quote.source_id = {1};
  quote.instrument_id = FutureId();
  quote.sequence = sequence;
  quote.bid_price = bid;
  quote.ask_price = ask;
  quote.bid_qty = bid_qty;
  quote.ask_qty = ask_qty;
  quote.flags = 7;
  return quote;
}

model::OrderRequest ExchangeOrder(std::int64_t limit = 3500,
                                  std::int64_t quantity = 1) {
  return {FutureId(), OrderSide::kBuy, limit, quantity, PositionEffect::kOpen};
}

TradingEvent PollEvent(SimulatedFuturesExchange& exchange, std::uint64_t now) {
  TradingEvent event;
  const auto result = exchange.Poll(now, event);
  EXPECT_EQ(result.state, TradingPollState::kEvent) << exchange.error_detail();
  EXPECT_TRUE(result.status.ok());
  return event;
}

TEST(FuturesExchangeTest,
     AggressiveOrdersShareQuoteLiquidityAndLaterFillsUseMakerLimits) {
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(exchange.OnQuote(ExchangeQuote(), 0).ok());
  ASSERT_TRUE(exchange.SendOrder({1}, ExchangeOrder(3502, 2), 0).ok());
  ASSERT_TRUE(exchange.SendOrder({2}, ExchangeOrder(3503, 2), 0).ok());
  TradingEvent untouched;
  untouched.report_sequence = 999;
  EXPECT_EQ(exchange.Poll(kInterval - 1, untouched).state,
            TradingPollState::kIdle);
  EXPECT_EQ(untouched.report_sequence, 999U);
  auto accepted = PollEvent(exchange, kInterval);
  EXPECT_EQ(accepted.client_order_id.value, 1U);
  EXPECT_EQ(accepted.kind, OrderUpdateKind::kAccepted);
  const auto first = PollEvent(exchange, kInterval);
  EXPECT_EQ(first.kind, OrderUpdateKind::kFill);
  EXPECT_EQ(first.fill.price, 3501);
  EXPECT_EQ(first.fill.quantity, 2);
  EXPECT_EQ(first.fill.position_effect, PositionEffect::kOpen);
  accepted = PollEvent(exchange, kInterval);
  EXPECT_EQ(accepted.client_order_id.value, 2U);
  const auto second = PollEvent(exchange, kInterval);
  EXPECT_EQ(second.fill.quantity, 1);
  EXPECT_EQ(second.fill.price, 3501);
  EXPECT_NE(first.fill.execution_id, second.fill.execution_id);
  EXPECT_FALSE(exchange.is_drained());
  auto repeated = ExchangeQuote();
  repeated.ask_qty = 100;
  ASSERT_TRUE(exchange.OnQuote(repeated, kInterval).ok());
  EXPECT_EQ(exchange.Poll(kInterval, untouched).state, TradingPollState::kIdle);
  ASSERT_TRUE(
      exchange.OnQuote(ExchangeQuote(2, 3499, 3500, 1, 1), 2 * kInterval).ok());
  const auto maker = PollEvent(exchange, 2 * kInterval);
  EXPECT_EQ(maker.client_order_id.value, 2U);
  EXPECT_EQ(maker.report_sequence, 3U);
  EXPECT_EQ(maker.fill.quantity, 1);
  EXPECT_EQ(maker.fill.price, 3503);
  EXPECT_EQ(maker.cumulative_filled_qty, 2);
  EXPECT_TRUE(exchange.is_drained());
}

TEST(FuturesExchangeTest,
     TimeAloneDoesNotFillAndNewAggressorCannotReuseConsumedVolume) {
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(exchange.SendOrder({1}, ExchangeOrder(), 0).ok());
  EXPECT_EQ(PollEvent(exchange, kInterval).kind, OrderUpdateKind::kAccepted);
  TradingEvent event;
  event.report_sequence = 999;
  EXPECT_EQ(exchange.Poll(100 * kInterval, event).state,
            TradingPollState::kIdle);
  EXPECT_EQ(event.report_sequence, 999U);
  ASSERT_TRUE(exchange.OnQuote(ExchangeQuote(), 101 * kInterval).ok());
  EXPECT_EQ(exchange.Poll(101 * kInterval, event).state,
            TradingPollState::kIdle);
  ASSERT_TRUE(
      exchange.OnQuote(ExchangeQuote(2, 3499, 3500, 1, 1), 102 * kInterval)
          .ok());
  EXPECT_EQ(PollEvent(exchange, 102 * kInterval).fill.price, 3500);
  ASSERT_TRUE(exchange.SendOrder({2}, ExchangeOrder(), 102 * kInterval).ok());
  const auto accepted = PollEvent(exchange, 103 * kInterval);
  EXPECT_EQ(accepted.kind, OrderUpdateKind::kAccepted);
  EXPECT_EQ(exchange.Poll(103 * kInterval, event).state,
            TradingPollState::kIdle);
  ASSERT_TRUE(
      exchange.SendCancel({2}, accepted.broker_order_id, {1}, 103 * kInterval)
          .ok());
  EXPECT_EQ(PollEvent(exchange, 104 * kInterval).kind,
            OrderUpdateKind::kCanceled);
  EXPECT_TRUE(exchange.is_drained());
}

class FuturesCancelRaceTest : public ::testing::TestWithParam<bool> {};
TEST_P(FuturesCancelRaceTest,
       MatchingBeforeCancelPublishesFillThenAssociatedResult) {
  const bool full = GetParam();
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(
      exchange.SendOrder({1}, ExchangeOrder(3500, full ? 1 : 3), 0).ok());
  const auto accepted = PollEvent(exchange, kInterval);
  ASSERT_TRUE(
      exchange.SendCancel({1}, accepted.broker_order_id, {7}, kInterval).ok());
  ASSERT_TRUE(
      exchange.OnQuote(ExchangeQuote(1, 3499, 3500, 1, 1), 1500000).ok());
  const auto fill = PollEvent(exchange, 1500000);
  EXPECT_EQ(fill.kind, OrderUpdateKind::kFill);
  EXPECT_EQ(fill.report_sequence, 2U);
  EXPECT_EQ(fill.fill.quantity, 1);
  const auto canceled = PollEvent(exchange, 2 * kInterval);
  EXPECT_EQ(canceled.kind, full ? OrderUpdateKind::kCancelRejected
                                : OrderUpdateKind::kCanceled);
  EXPECT_EQ(canceled.cancel_request_id.value, 7U);
  EXPECT_EQ(canceled.cumulative_filled_qty, 1);
  EXPECT_EQ(canceled.report_sequence, 3U);
  ASSERT_TRUE(
      exchange.OnQuote(ExchangeQuote(2, 3498, 3499), 3 * kInterval).ok());
  TradingEvent unchanged;
  EXPECT_EQ(exchange.Poll(3 * kInterval, unchanged).state,
            TradingPollState::kIdle);
  EXPECT_TRUE(exchange.is_drained());
}
INSTANTIATE_TEST_SUITE_P(CancelRaces, FuturesCancelRaceTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "Filled" : "PartiallyFilled";
                         });

TEST(FuturesExchangeTest, CancelEffectiveBoundaryPreventsAnotherQuoteFill) {
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(exchange.SendOrder({1}, ExchangeOrder(), 0).ok());
  const auto accepted = PollEvent(exchange, kInterval);
  ASSERT_TRUE(
      exchange.SendCancel({1}, accepted.broker_order_id, {1}, kInterval).ok());
  ASSERT_TRUE(
      exchange.OnQuote(ExchangeQuote(1, 3499, 3500), 2 * kInterval).ok());
  const auto canceled = PollEvent(exchange, 2 * kInterval);
  EXPECT_EQ(canceled.kind, OrderUpdateKind::kCanceled);
  EXPECT_EQ(canceled.cumulative_filled_qty, 0);
  EXPECT_TRUE(exchange.is_drained());
}

TEST(FuturesExchangeTest,
     BootstrapAndInvalidPollPreserveOutputAndUnconfirmedState) {
  SimulatedFuturesExchange exchange;
  TradingAccountSnapshot account;
  account.cash_balance = 17;
  EXPECT_FALSE(exchange.ReadAccountSnapshot(account).ok());
  EXPECT_EQ(account.cash_balance, 17);
  const auto config = ExchangeConfig();
  ASSERT_TRUE(exchange.Prepare(config).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(exchange.ReadAccountSnapshot(account).ok());
  EXPECT_EQ(account, config.initial_account);
  ASSERT_TRUE(exchange.SendOrder({1}, ExchangeOrder(), 0).ok());
  account.cash_balance = 17;
  EXPECT_FALSE(exchange.ReadAccountSnapshot(account).ok());
  EXPECT_EQ(account.cash_balance, 17);
  ASSERT_TRUE(exchange.Stop().ok());
  EXPECT_FALSE(exchange.is_drained());
  TradingEvent output;
  output.report_sequence = 999;
  EXPECT_EQ(exchange.Poll(kInterval, output).state, TradingPollState::kError);
  EXPECT_EQ(output.report_sequence, 999U);
  EXPECT_FALSE(exchange.is_drained());
  EXPECT_TRUE(exchange.Stop().ok());
}

TEST(FuturesExchangeTest, FillHistoryCapacityFailureDoesNotConsumeTheLastLot) {
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(
      exchange.SendOrder({1}, ExchangeOrder(3500, kMaxTradingFills + 1), 0)
          .ok());
  const auto accepted = PollEvent(exchange, kInterval);
  for (std::size_t index = 0; index < kMaxTradingFills; ++index) {
    const auto now = kInterval + index;
    ASSERT_TRUE(
        exchange.OnQuote(ExchangeQuote(index + 1, 3499, 3500, 1, 1), now).ok());
    const auto fill = PollEvent(exchange, now);
    ASSERT_EQ(fill.fill.quantity, 1);
    ASSERT_EQ(fill.cumulative_filled_qty, static_cast<std::int64_t>(index + 1));
  }
  const auto now = 2 * kInterval;
  EXPECT_EQ(
      exchange
          .OnQuote(ExchangeQuote(kMaxTradingFills + 1, 3499, 3500, 1, 1), now)
          .code,
      ErrorCode::kResourceExhausted);
  TradingEvent output;
  EXPECT_EQ(exchange.Poll(now, output).state, TradingPollState::kIdle);
  ASSERT_TRUE(
      exchange.SendCancel({1}, accepted.broker_order_id, {1}, now).ok());
  const auto canceled = PollEvent(exchange, now + kInterval);
  EXPECT_EQ(canceled.kind, OrderUpdateKind::kCanceled);
  EXPECT_EQ(canceled.cumulative_filled_qty,
            static_cast<std::int64_t>(kMaxTradingFills));
  EXPECT_TRUE(exchange.is_drained());
}

TEST(FuturesExchangeTest, AnotherSourceHasItsOwnQuoteSequence) {
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(ExchangeConfig()).ok());
  ASSERT_TRUE(exchange.Start(0).ok());
  ASSERT_TRUE(exchange.OnQuote(ExchangeQuote(100), 0).ok());
  ASSERT_TRUE(exchange.SendOrder({1}, ExchangeOrder(), 0).ok());
  ASSERT_EQ(PollEvent(exchange, kInterval).kind, OrderUpdateKind::kAccepted);
  auto other_source = ExchangeQuote(1, 3499, 3500);
  other_source.source_id = {2};
  ASSERT_TRUE(exchange.OnQuote(other_source, kInterval).ok());
  const auto fill = PollEvent(exchange, kInterval);
  EXPECT_EQ(fill.kind, OrderUpdateKind::kFill);
  EXPECT_EQ(fill.fill.price, 3500);
  EXPECT_EQ(fill.fill.quantity, 1);
  EXPECT_TRUE(exchange.is_drained());
}

}  // namespace
}  // namespace yquant::strategyd
