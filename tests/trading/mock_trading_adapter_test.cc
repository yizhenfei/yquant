#include "yquant/trading/mock_trading_adapter.h"

#include <array>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {
namespace {

constexpr std::uint64_t kInterval = 1000000;
static_assert(sizeof(trading::MockTradingAdapter) < 1024);

model::OrderRequest Request(std::int64_t quantity = 200) {
  return {{(1ULL << 32) | 600000}, model::OrderSide::kBuy, 1000, quantity};
}

bool Start(trading::MockTradingAdapter& adapter,
           trading::MockTradingMode mode = trading::MockTradingMode::kFill) {
  trading::TradingConfig config;
  config.mode = mode;
  return adapter.Prepare(config).ok() && adapter.Start(0).ok();
}

// Compares fields rather than padding, including values unused by event kind.
bool SameEvent(const model::TradingEvent& left,
               const model::TradingEvent& right) {
  const auto& a = left.fill;
  const auto& b = right.fill;
  return left.client_order_id == right.client_order_id &&
         left.broker_order_id == right.broker_order_id &&
         left.report_sequence == right.report_sequence &&
         left.kind == right.kind &&
         left.cumulative_filled_qty == right.cumulative_filled_qty &&
         left.receive_time_ns == right.receive_time_ns &&
         left.cancel_request_id == right.cancel_request_id &&
         left.reason.code == right.reason.code &&
         left.reason.view() == right.reason.view() &&
         left.reason.truncated == right.reason.truncated &&
         a.client_order_id == b.client_order_id &&
         a.broker_order_id == b.broker_order_id &&
         a.execution_id == b.execution_id &&
         a.instrument_id == b.instrument_id && a.side == b.side &&
         a.price == b.price && a.quantity == b.quantity &&
         a.event_time_ns == b.event_time_ns &&
         a.receive_time_ns == b.receive_time_ns &&
         a.has_event_time == b.has_event_time;
}

TEST(MockTradingAdapterTest, FillsInTwoStepsWithOwnedIdsAndContinuousSequence) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  EXPECT_TRUE(adapter.is_drained());
  auto request = Request();
  ASSERT_TRUE(adapter.SendOrder({1}, request, 0).ok());
  request = {};
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_EQ(adapter.next_deadline(), kInterval);
  model::TradingEvent event;
  event.client_order_id = {99};
  auto unchanged = event;
  EXPECT_EQ(adapter.Poll(kInterval - 1, event).state,
            trading::TradingPollState::kIdle);
  EXPECT_TRUE(SameEvent(event, unchanged));
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kAccepted);
  EXPECT_EQ(event.client_order_id.value, 1U);
  EXPECT_EQ(event.report_sequence, 1U);
  EXPECT_EQ(event.cumulative_filled_qty, 0);
  EXPECT_FALSE(event.broker_order_id.empty());
  EXPECT_EQ(event.fill.client_order_id.value, 0U);
  const auto broker = event.broker_order_id;
  unchanged = event;
  EXPECT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kIdle);
  EXPECT_TRUE(SameEvent(event, unchanged));
  ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kFill);
  EXPECT_EQ(event.report_sequence, 2U);
  EXPECT_EQ(event.cumulative_filled_qty, 100);
  EXPECT_EQ(event.broker_order_id, broker);
  EXPECT_EQ(event.fill.broker_order_id, broker);
  EXPECT_EQ(event.fill.client_order_id.value, 1U);
  EXPECT_EQ(event.fill.instrument_id, Request().instrument_id);
  EXPECT_EQ(event.fill.side, model::OrderSide::kBuy);
  EXPECT_EQ(event.fill.price, 1000);
  EXPECT_EQ(event.fill.quantity, 100);
  EXPECT_EQ(event.fill.receive_time_ns, 2 * kInterval);
  EXPECT_TRUE(event.fill.has_event_time);
  EXPECT_GT(event.fill.event_time_ns, 0U);
  const auto execution = event.fill.execution_id;
  EXPECT_FALSE(execution.empty());
  ASSERT_EQ(adapter.Poll(3 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kFill);
  EXPECT_EQ(event.report_sequence, 3U);
  EXPECT_EQ(event.cumulative_filled_qty, 200);
  EXPECT_EQ(event.fill.quantity, 100);
  EXPECT_NE(event.fill.execution_id, execution);
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
}

TEST(MockTradingAdapterTest, OneShareSkipsZeroFillAndOddQuantityUsesFloor) {
  constexpr std::array<std::int64_t, 3> kQuantities = {
      1, 3, std::numeric_limits<std::int64_t>::max()};
  for (const auto quantity : kQuantities) {
    trading::MockTradingAdapter adapter;
    ASSERT_TRUE(Start(adapter));
    ASSERT_TRUE(adapter.SendOrder({1}, Request(quantity), 0).ok());
    model::TradingEvent event;
    ASSERT_EQ(adapter.Poll(kInterval, event).state,
              trading::TradingPollState::kEvent);
    ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
              trading::TradingPollState::kEvent);
    EXPECT_EQ(event.kind, model::OrderUpdateKind::kFill);
    EXPECT_EQ(event.fill.quantity, quantity == 1 ? 1 : quantity / 2);
    if (quantity != 1) {
      ASSERT_EQ(adapter.Poll(3 * kInterval, event).state,
                trading::TradingPollState::kEvent);
      EXPECT_EQ(event.fill.quantity, quantity - quantity / 2);
    }
    EXPECT_EQ(event.cumulative_filled_qty, quantity);
    EXPECT_TRUE(adapter.is_drained());
  }
}

TEST(MockTradingAdapterTest, WorkingRequiresExplicitCancellation) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter, trading::MockTradingMode::kWorking));
  ASSERT_TRUE(adapter.SendOrder({7}, Request(), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
  const auto broker = event.broker_order_id;
  ASSERT_TRUE(adapter.SendCancel({7}, broker, {11}, kInterval).ok());
  ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCanceled);
  EXPECT_EQ(event.cancel_request_id.value, 11U);
  EXPECT_EQ(event.report_sequence, 2U);
  EXPECT_EQ(event.cumulative_filled_qty, 0);
  EXPECT_EQ(event.fill.quantity, 0);
  EXPECT_TRUE(adapter.is_drained());
}

TEST(MockTradingAdapterTest, RejectModeTerminatesWithoutFills) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter, trading::MockTradingMode::kReject));
  ASSERT_TRUE(adapter.SendOrder({4}, Request(), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kRejected);
  EXPECT_EQ(event.report_sequence, 1U);
  EXPECT_NE(event.reason.code, 0);
  EXPECT_FALSE(event.reason.view().empty());
  EXPECT_EQ(event.fill.quantity, 0);
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
}

TEST(MockTradingAdapterTest, EqualDeadlinesFollowPlanRegistrationOrder) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({9}, Request(), 0).ok());
  ASSERT_TRUE(adapter.SendOrder({3}, Request(), 0).ok());
  model::TradingEvent event;
  for (const auto now : {kInterval, 2 * kInterval, 3 * kInterval}) {
    ASSERT_EQ(adapter.Poll(now, event).state,
              trading::TradingPollState::kEvent);
    EXPECT_EQ(event.client_order_id.value, 9U);
    ASSERT_EQ(adapter.Poll(now, event).state,
              trading::TradingPollState::kEvent);
    EXPECT_EQ(event.client_order_id.value, 3U);
  }
  EXPECT_TRUE(adapter.is_drained());
}

TEST(MockTradingAdapterTest, LatePollingDoesNotFastForwardFutureSteps) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(100 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kAccepted);
  EXPECT_EQ(adapter.next_deadline(), 101 * kInterval);
  EXPECT_EQ(adapter.Poll(100 * kInterval, event).state,
            trading::TradingPollState::kIdle);
  ASSERT_EQ(adapter.Poll(200 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.fill.quantity, 100);
  EXPECT_EQ(adapter.next_deadline(), 201 * kInterval);
  EXPECT_EQ(adapter.Poll(200 * kInterval, event).state,
            trading::TradingPollState::kIdle);
}

TEST(MockTradingAdapterTest, DueFillPrecedesCancelAndFutureFillIsRemoved) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  const auto broker = event.broker_order_id;
  ASSERT_TRUE(adapter.SendCancel({1}, broker, {9}, kInterval).ok());
  ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kFill);
  EXPECT_EQ(event.cumulative_filled_qty, 100);
  ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCanceled);
  EXPECT_EQ(event.cancel_request_id.value, 9U);
  EXPECT_EQ(event.report_sequence, 3U);
  EXPECT_EQ(event.cumulative_filled_qty, 100);
  EXPECT_EQ(event.fill.quantity, 0);
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
  const auto unchanged = event;
  EXPECT_EQ(adapter.Poll(100 * kInterval, event).state,
            trading::TradingPollState::kIdle);
  EXPECT_TRUE(SameEvent(event, unchanged));
}

TEST(MockTradingAdapterTest,
     CancelSentAfterFullFillStillProducesAttemptResult) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  ASSERT_TRUE(adapter.SendOrder({1}, Request(1), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  const auto broker = event.broker_order_id;
  ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  ASSERT_TRUE(adapter.is_drained());
  ASSERT_TRUE(adapter.SendCancel({1}, broker, {77}, 2 * kInterval).ok());
  EXPECT_FALSE(adapter.is_drained());
  ASSERT_EQ(adapter.Poll(3 * kInterval, event).state,
            trading::TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCancelRejected);
  EXPECT_EQ(event.cancel_request_id.value, 77U);
  EXPECT_EQ(event.report_sequence, 3U);
  EXPECT_EQ(event.cumulative_filled_qty, 1);
  EXPECT_TRUE(adapter.is_drained());
}

TEST(MockTradingAdapterTest, ValidatesSendsWithoutSchedulingFailedRequests) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  auto request = Request();
  EXPECT_FALSE(adapter.SendOrder({}, request, 0).ok());
  request.side = model::OrderSide::kUnknown;
  EXPECT_FALSE(adapter.SendOrder({1}, request, 0).ok());
  request = Request(0);
  EXPECT_FALSE(adapter.SendOrder({1}, request, 0).ok());
  request = Request();
  request.limit_price = 0;
  EXPECT_FALSE(adapter.SendOrder({1}, request, 0).ok());
  request = Request();
  request.instrument_id = {};
  EXPECT_FALSE(adapter.SendOrder({1}, request, 0).ok());
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  EXPECT_FALSE(adapter.SendOrder({1}, Request(), 0).ok());
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  const auto broker = event.broker_order_id;
  EXPECT_FALSE(adapter.SendCancel({1}, broker, {}, kInterval).ok());
  EXPECT_FALSE(adapter.SendCancel({99}, broker, {1}, kInterval).ok());
  model::BrokerOrderId incorrect;
  ASSERT_EQ(incorrect.Assign("another-order"), base::ErrorCode::kOk);
  EXPECT_FALSE(adapter.SendCancel({1}, incorrect, {1}, kInterval).ok());
  ASSERT_TRUE(adapter.SendCancel({1}, broker, {1}, kInterval).ok());
  EXPECT_FALSE(adapter.SendCancel({1}, broker, {1}, kInterval).ok());
  EXPECT_FALSE(adapter.SendCancel({1}, broker, {2}, kInterval).ok());
  EXPECT_EQ(adapter.next_deadline(), 2 * kInterval);
}

TEST(MockTradingAdapterTest, FullOrderAndPlanCapacityDrainsWithoutStaleNodes) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  std::array<model::BrokerOrderId, trading::kMaxTradingOrders> brokers;
  for (std::size_t index = 0; index < trading::kMaxTradingOrders; ++index) {
    ASSERT_TRUE(adapter.SendOrder({index + 1}, Request(), 0).ok());
  }
  EXPECT_EQ(
      adapter.SendOrder({trading::kMaxTradingOrders + 1}, Request(), 0).code,
      base::ErrorCode::kResourceExhausted);
  model::TradingEvent event;
  for (std::size_t index = 0; index < trading::kMaxTradingOrders; ++index) {
    ASSERT_EQ(adapter.Poll(kInterval, event).state,
              trading::TradingPollState::kEvent);
    ASSERT_EQ(event.client_order_id.value, index + 1);
    brokers[index] = event.broker_order_id;
  }
  for (std::size_t index = 0; index < trading::kMaxTradingOrders; ++index) {
    ASSERT_TRUE(
        adapter.SendCancel({index + 1}, brokers[index], {index + 1}, kInterval)
            .ok());
  }
  for (std::size_t index = 0; index < trading::kMaxTradingOrders; ++index) {
    ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
              trading::TradingPollState::kEvent);
    ASSERT_EQ(event.kind, model::OrderUpdateKind::kFill);
    EXPECT_EQ(event.client_order_id.value, index + 1);
  }
  for (std::size_t index = 0; index < trading::kMaxTradingOrders; ++index) {
    ASSERT_EQ(adapter.Poll(2 * kInterval, event).state,
              trading::TradingPollState::kEvent);
    ASSERT_EQ(event.kind, model::OrderUpdateKind::kCanceled);
    EXPECT_EQ(event.client_order_id.value, index + 1);
    EXPECT_EQ(event.cancel_request_id.value, index + 1);
    EXPECT_EQ(event.cumulative_filled_qty, 100);
  }
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
}

TEST(MockTradingAdapterTest,
     ChecksDeadlineOverflowBeforePublishingOrScheduling) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter));
  EXPECT_EQ(adapter.SendOrder({1}, Request(), UINT64_MAX - kInterval + 1).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_TRUE(adapter.is_drained());
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), UINT64_MAX - kInterval).ok());
  model::TradingEvent event;
  event.client_order_id = {999};
  const auto unchanged = event;
  const auto poll = adapter.Poll(UINT64_MAX, event);
  EXPECT_EQ(poll.state, trading::TradingPollState::kError);
  EXPECT_EQ(poll.status.code, base::ErrorCode::kResourceExhausted);
  EXPECT_TRUE(SameEvent(event, unchanged));
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
  ASSERT_TRUE(adapter.Stop().ok());
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_NE(adapter.error_detail()[0], '\0');
}

TEST(MockTradingAdapterTest,
     StateErrorsPreserveOutputAndStopDoesNotClaimCancellation) {
  trading::MockTradingAdapter adapter;
  EXPECT_TRUE(adapter.is_drained());
  EXPECT_FALSE(adapter.Start(0).ok());
  EXPECT_FALSE(adapter.SendOrder({1}, Request(), 0).ok());
  model::TradingEvent event;
  event.client_order_id = {9};
  const auto unchanged = event;
  EXPECT_EQ(adapter.Poll(0, event).state, trading::TradingPollState::kError);
  EXPECT_TRUE(SameEvent(event, unchanged));
  trading::TradingConfig invalid;
  invalid.report_interval_ns = 0;
  EXPECT_FALSE(adapter.Prepare(invalid).ok());
  ASSERT_TRUE(Start(adapter, trading::MockTradingMode::kWorking));
  EXPECT_FALSE(adapter.Prepare({}).ok());
  EXPECT_FALSE(adapter.Start(0).ok());
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  ASSERT_TRUE(adapter.Stop().ok());
  EXPECT_TRUE(adapter.Stop().ok());
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_FALSE(adapter.next_deadline());
  EXPECT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kError);
  EXPECT_TRUE(SameEvent(event, unchanged));
}

TEST(MockTradingAdapterTest, AccountBootstrapOwnsConfigAndCopiesOutput) {
  trading::TradingConfig config;
  config.initial_account.cash_balance = 100000000000000LL;
  config.initial_account.position_count = 1;
  config.initial_account.positions[0] = {Request().instrument_id, 1000, 800, 0};
  const auto expected = config.initial_account;
  trading::MockTradingAdapter adapter;
  trading::TradingAccountSnapshot snapshot;
  snapshot.cash_balance = 123;
  snapshot.position_count = 1;
  snapshot.positions[0] = {Request().instrument_id, 7, 5, 0};
  const auto unchanged = snapshot;
  EXPECT_EQ(adapter.ReadAccountSnapshot(snapshot).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(snapshot, unchanged);
  ASSERT_TRUE(adapter.Prepare(config).ok());
  EXPECT_EQ(adapter.ReadAccountSnapshot(snapshot).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(snapshot, unchanged);
  config.initial_account = {};
  ASSERT_TRUE(adapter.Start(0).ok());
  ASSERT_TRUE(adapter.ReadAccountSnapshot(snapshot).ok());
  EXPECT_EQ(snapshot, expected);
  snapshot.positions[0].total_qty = 2;
  snapshot.cash_balance = 0;
  trading::TradingAccountSnapshot second;
  ASSERT_TRUE(adapter.ReadAccountSnapshot(second).ok());
  EXPECT_EQ(second, expected);
  EXPECT_TRUE(adapter.Stop().ok());
  const auto after_stop = snapshot;
  EXPECT_EQ(adapter.ReadAccountSnapshot(snapshot).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(snapshot, after_stop);
}

TEST(MockTradingAdapterTest,
     BootstrapClosesAfterFirstAcceptedOrderEvenIfDrained) {
  trading::MockTradingAdapter adapter;
  ASSERT_TRUE(Start(adapter, trading::MockTradingMode::kReject));
  trading::TradingAccountSnapshot snapshot;
  ASSERT_TRUE(adapter.ReadAccountSnapshot(snapshot).ok());
  EXPECT_EQ(snapshot.cash_balance, 0);
  EXPECT_EQ(snapshot.position_count, 0U);
  EXPECT_FALSE(adapter.SendOrder({}, Request(), 0).ok());
  EXPECT_TRUE(adapter.ReadAccountSnapshot(snapshot).ok());
  ASSERT_TRUE(adapter.SendOrder({1}, Request(), 0).ok());
  snapshot.cash_balance = 89;
  const auto unchanged = snapshot;
  EXPECT_EQ(adapter.ReadAccountSnapshot(snapshot).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(snapshot, unchanged);
  model::TradingEvent event;
  ASSERT_EQ(adapter.Poll(kInterval, event).state,
            trading::TradingPollState::kEvent);
  ASSERT_TRUE(adapter.is_drained());
  EXPECT_EQ(adapter.ReadAccountSnapshot(snapshot).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(snapshot, unchanged);
}

TEST(MockTradingAdapterTest, InvalidBootstrapAccountDoesNotPrepareCounter) {
  trading::MockTradingAdapter adapter;
  trading::TradingConfig config;
  config.initial_account.cash_balance = -1;
  EXPECT_FALSE(adapter.Prepare(config).ok());
  EXPECT_FALSE(adapter.Start(0).ok());
  config.initial_account.cash_balance = 0;
  config.initial_account.position_count = 1;
  config.initial_account.positions[0] = {Request().instrument_id, 100, 100, 1};
  EXPECT_FALSE(adapter.Prepare(config).ok());
  config.initial_account.positions[0].reserved_qty = 0;
  ASSERT_TRUE(adapter.Prepare(config).ok());
  ASSERT_TRUE(adapter.Start(0).ok());
  trading::TradingAccountSnapshot snapshot;
  ASSERT_TRUE(adapter.ReadAccountSnapshot(snapshot).ok());
  EXPECT_EQ(snapshot, config.initial_account);
}

}  // namespace
}  // namespace yquant::trading
