#include "yquant/trading/order_request_executor.h"

#include <stdint.h>

#include <array>

#include "gtest/gtest.h"
#include "yquant/base/status.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;

class OrderRequestExecutorTest : public ::testing::Test {
 protected:
  // Registers a fresh dense slot using deliberately sparse trading IDs.
  void QueueNew(size_t index) {
    const auto prepared = executor.PrepareQueueNew(index, {1000 + index});
    ASSERT_TRUE(prepared.ok()) << prepared.detail;
    executor.Commit(prepared);
  }

  // Commits one send attempt without invoking any external transport.
  OrderExecutionRequest Send(uint64_t now = 100) {
    const auto request = executor.front();
    EXPECT_TRUE(request.has_value());
    const auto prepared = executor.PrepareBeginSend(now);
    EXPECT_TRUE(prepared.ok()) << prepared.detail;
    if (prepared.ok()) {
      executor.Commit(prepared);
    }
    return request.value_or(OrderExecutionRequest{});
  }

  // Builds only association fields; OrderCore owns report business validation.
  model::TradingEvent Report(size_t index, model::OrderUpdateKind kind) const {
    model::TradingEvent event;
    event.client_order_id = {1000 + index};
    event.kind = kind;
    event.receive_time_ns = 150;
    return event;
  }

  // Applies a broker acceptance so later cancel commands are legitimate.
  void Accept(size_t index) {
    const auto event = Report(index, model::OrderUpdateKind::kAccepted);
    const auto prepared = executor.PrepareReport(index, event, Snapshot(index));
    ASSERT_TRUE(prepared.ok()) << prepared.detail;
    executor.Commit(prepared);
  }

  // Builds a candidate core view supplied to report preparation.
  model::OrderSnapshot Snapshot(
      size_t index,
      model::OrderState state = model::OrderState::kWorking) const {
    model::OrderSnapshot order;
    order.client_order_id = {1000 + index};
    order.state = state;
    return order;
  }

  // Registers the named cancel for an already accepted order.
  void Cancel(size_t index, uint64_t cancel_id = 1) {
    const auto prepared =
        executor.PrepareQueueCancel(index, {1000 + index}, {cancel_id});
    ASSERT_TRUE(prepared.ok()) << prepared.detail;
    executor.Commit(prepared);
  }

  OrderRequestExecutor executor;
};

TEST_F(OrderRequestExecutorTest, PreparationPreservesStateUntilCommit) {
  const auto before = executor.state();
  const auto queued = executor.PrepareQueueNew(0, {1000});
  ASSERT_TRUE(queued.ok());
  EXPECT_EQ(executor.state(), before);
  executor.Commit(queued);
  EXPECT_EQ(executor.order_count(), 1U);
  EXPECT_EQ(executor.request_count(), 1U);
  EXPECT_EQ(executor.front()->client_order_id.value, 1000U);
  const auto sending = executor.PrepareBeginSend(200);
  ASSERT_TRUE(sending.ok());
  EXPECT_EQ(executor.state().executions[0].new_request.state,
            OrderRequestState::kQueued);
  executor.Commit(sending);
  EXPECT_EQ(executor.request_count(), 0U);
  const auto& progress = executor.state().executions[0].new_request;
  EXPECT_EQ(progress.state, OrderRequestState::kInFlight);
  EXPECT_TRUE(progress.attempted);
  EXPECT_EQ(progress.attempted_at_ns, 200U);
  EXPECT_EQ(progress.response_deadline_ns, 60000000200U);
  EXPECT_FALSE(executor.is_drained());
}

TEST_F(OrderRequestExecutorTest, FirstBrokerResultEndsNewRequestOnly) {
  QueueNew(0);
  Send();
  const auto event = Report(0, model::OrderUpdateKind::kFill);
  auto prepared = executor.PrepareReport(0, event, Snapshot(0));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_EQ(executor.state().executions[0].new_request.state,
            OrderRequestState::kCompleted);
  EXPECT_FALSE(executor.next_deadline());
  EXPECT_TRUE(executor.is_drained());
  prepared =
      executor.PrepareReport(0, event, Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
  prepared =
      executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kAccepted),
                             Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
}

TEST_F(OrderRequestExecutorTest, ResponseRequiresMatchingAttemptedRequest) {
  QueueNew(0);
  const auto before = executor.state();
  EXPECT_EQ(executor
                .PrepareReport(0, Report(0, model::OrderUpdateKind::kAccepted),
                               Snapshot(0))
                .code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(executor.state(), before);
  Send();
  Accept(0);
  Cancel(0, 91);
  auto response = Report(0, model::OrderUpdateKind::kCancelRejected);
  response.cancel_request_id = {91};
  EXPECT_EQ(executor.PrepareReport(0, response, Snapshot(0)).code,
            ErrorCode::kCorrupt);
  Send(200);
  response.cancel_request_id = {92};
  EXPECT_EQ(executor.PrepareReport(0, response, Snapshot(0)).code,
            ErrorCode::kCorrupt);
  response.cancel_request_id = {91};
  const auto prepared = executor.PrepareReport(0, response, Snapshot(0));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
  EXPECT_EQ(executor.PrepareReport(0, response, Snapshot(0)).code,
            ErrorCode::kCorrupt);
}

TEST_F(OrderRequestExecutorTest,
       FullFillRemovesQueuedCancelAndDefersLocalResult) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 7);
  QueueNew(1);
  auto order = Snapshot(0, model::OrderState::kFilled);
  order.cancel_pending = true;
  order.cancel_request_id = {7};
  const auto before = executor.state();
  const auto prepared = executor.PrepareReport(
      0, Report(0, model::OrderUpdateKind::kFill), order);
  ASSERT_TRUE(prepared.ok());
  EXPECT_EQ(executor.state(), before);
  executor.Commit(prepared);
  EXPECT_EQ(executor.request_count(), 1U);
  EXPECT_EQ(executor.front()->order_index, 1U);
  ASSERT_EQ(executor.local_count(), 1U);
  EXPECT_EQ(executor.local_front()->kind, OrderLocalResultKind::kCancelNotSent);
  EXPECT_EQ(executor.local_front()->request.cancel_request_id.value, 7U);
  EXPECT_TRUE(executor.state().executions[0].cancel_local_result_pending);
  const auto consumed = executor.PrepareConsumeLocalResult();
  ASSERT_TRUE(consumed.ok());
  executor.Commit(consumed);
  EXPECT_EQ(executor.local_count(), 0U);
  EXPECT_FALSE(executor.state().executions[0].cancel_local_result_pending);
  EXPECT_EQ(executor.PrepareConsumeLocalResult().code,
            ErrorCode::kInvalidState);
}

TEST_F(OrderRequestExecutorTest, LegacyReplayKeepsQueuedCancelAfterFullFill) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0);
  const auto prepared =
      executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kFill),
                             Snapshot(0, model::OrderState::kFilled), false);
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_EQ(executor.request_count(), 1U);
  EXPECT_EQ(executor.local_count(), 0U);
  EXPECT_EQ(Send().kind, OrderRequestKind::kCancel);
}

TEST_F(OrderRequestExecutorTest, FullFillKeepsAttemptedCancelUntilItsResult) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 29);
  Send(200);
  auto prepared =
      executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kFill),
                             Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_EQ(executor.state().executions[0].cancel_request.state,
            OrderRequestState::kInFlight);
  EXPECT_EQ(executor.local_count(), 0U);
  EXPECT_FALSE(executor.is_drained());
  auto response = Report(0, model::OrderUpdateKind::kCancelRejected);
  response.cancel_request_id = {29};
  prepared = executor.PrepareReport(0, response,
                                    Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
}

TEST_F(OrderRequestExecutorTest, FullFillPreservesAnUncertainCancel) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 31);
  const auto request = Send(200);
  auto prepared = executor.PrepareUncertain(request);
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  prepared = executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kFill),
                                    Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_EQ(executor.state().executions[0].cancel_request.state,
            OrderRequestState::kUncertain);
  EXPECT_FALSE(executor.local_front());
  EXPECT_FALSE(executor.is_drained());
  auto response = Report(0, model::OrderUpdateKind::kCancelRejected);
  response.cancel_request_id = {31};
  prepared = executor.PrepareReport(0, response,
                                    Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
}

TEST_F(OrderRequestExecutorTest, OldCancelResultCannotCompleteANewerAttempt) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 41);
  Send(200);
  auto response = Report(0, model::OrderUpdateKind::kCancelRejected);
  response.cancel_request_id = {41};
  auto prepared = executor.PrepareReport(0, response, Snapshot(0));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  Cancel(0, 42);
  Send(300);
  const auto before = executor.state();
  EXPECT_EQ(executor.PrepareReport(0, response, Snapshot(0)).code,
            ErrorCode::kCorrupt);
  EXPECT_EQ(executor.state(), before);
  EXPECT_EQ(executor.state().executions[0].cancel_request.state,
            OrderRequestState::kInFlight);
}

TEST_F(OrderRequestExecutorTest, ShutdownQueuesLocalNewResultWithoutAnAttempt) {
  QueueNew(0);
  const auto prepared = executor.PrepareCancelBeforeSend(0, 700);
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_FALSE(executor.front());
  ASSERT_TRUE(executor.local_front());
  EXPECT_EQ(executor.local_front()->kind,
            OrderLocalResultKind::kCanceledBeforeSend);
  EXPECT_EQ(executor.local_front()->time_ns, 700U);
  EXPECT_FALSE(executor.state().executions[0].new_request.attempted);
  EXPECT_FALSE(executor.is_drained());
  EXPECT_EQ(executor.PrepareCancelBeforeSend(0, 701).code,
            ErrorCode::kInvalidState);
  const auto consumed = executor.PrepareConsumeLocalResult();
  ASSERT_TRUE(consumed.ok());
  executor.Commit(consumed);
  EXPECT_TRUE(executor.is_drained());
  EXPECT_EQ(executor
                .PrepareReport(0, Report(0, model::OrderUpdateKind::kAccepted),
                               Snapshot(0))
                .code,
            ErrorCode::kCorrupt);
}

TEST_F(OrderRequestExecutorTest,
       TimeoutAndSendFailureRemainUncertainWithoutRetry) {
  ASSERT_EQ(executor.Configure(50), ErrorCode::kOk);
  QueueNew(0);
  const auto request = Send(100);
  EXPECT_FALSE(executor.due(149));
  ASSERT_TRUE(executor.due(150));
  EXPECT_EQ(*executor.due(150), request);
  EXPECT_EQ(*executor.next_deadline(), 150U);
  auto prepared = executor.PrepareUncertain(request);
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_FALSE(executor.front());
  EXPECT_FALSE(executor.due(UINT64_MAX));
  EXPECT_FALSE(executor.next_deadline());
  EXPECT_FALSE(executor.is_drained());
  EXPECT_EQ(executor.PrepareCancelBeforeSend(0, 200).code,
            ErrorCode::kInvalidState);
  prepared = executor.PrepareReport(
      0, Report(0, model::OrderUpdateKind::kAccepted), Snapshot(0));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  EXPECT_TRUE(executor.is_drained());
}

TEST_F(OrderRequestExecutorTest, DeadlineOrderIsIndependentOfQueueWrap) {
  ASSERT_EQ(executor.Configure(50), ErrorCode::kOk);
  QueueNew(0);
  Send(100);
  QueueNew(1);
  Send(90);
  EXPECT_EQ(executor.due(140)->order_index, 1U);
  EXPECT_EQ(*executor.next_deadline(), 140U);
  Accept(1);
  EXPECT_FALSE(executor.due(149));
  EXPECT_EQ(executor.due(150)->order_index, 0U);
}

TEST_F(OrderRequestExecutorTest,
       QueuedCapacityIsReusableAfterRemovingMiddleCancel) {
  QueueNew(0);
  Send();
  Accept(0);
  QueueNew(1);
  Cancel(0, 7);
  for (size_t i = 2; i < 256; ++i) {
    QueueNew(i);
  }
  ASSERT_EQ(executor.request_count(), 256U);
  const auto full = executor.state();
  EXPECT_EQ(executor.PrepareQueueNew(256, {1256}).code,
            ErrorCode::kResourceExhausted);
  EXPECT_EQ(executor.state(), full);
  const auto prepared =
      executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kFill),
                             Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(prepared.ok());
  executor.Commit(prepared);
  QueueNew(256);
  EXPECT_EQ(executor.request_count(), 256U);
  for (size_t i = 1; i <= 256; ++i) {
    EXPECT_EQ(Send().order_index, i);
  }
  EXPECT_FALSE(executor.front());
}

TEST_F(OrderRequestExecutorTest, FullLocalQueueRejectsTheWholeFillTransition) {
  for (size_t i = 0; i < 256; ++i) {
    QueueNew(i);
    const auto local = executor.PrepareCancelBeforeSend(i, 200);
    ASSERT_TRUE(local.ok());
    executor.Commit(local);
  }
  QueueNew(256);
  Send();
  Accept(256);
  Cancel(256, 17);
  const auto before = executor.state();
  const auto prepared =
      executor.PrepareReport(256, Report(256, model::OrderUpdateKind::kFill),
                             Snapshot(256, model::OrderState::kFilled));
  EXPECT_EQ(prepared.code, ErrorCode::kResourceExhausted);
  EXPECT_EQ(executor.state(), before);
  EXPECT_EQ(executor.front()->cancel_request_id.value, 17U);
}

TEST_F(OrderRequestExecutorTest,
       RestoreValidatesQueuesAndPreservesStateOnFailure) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 5);
  QueueNew(1);
  const auto local = executor.PrepareCancelBeforeSend(1, 200);
  ASSERT_TRUE(local.ok());
  executor.Commit(local);
  OrderRequestExecutor restored;
  ASSERT_EQ(restored.RestoreState(executor.state()), ErrorCode::kOk);
  EXPECT_EQ(restored.state(), executor.state());
  auto broken = restored.state();
  broken.request_count = 0;
  EXPECT_EQ(restored.RestoreState(broken), ErrorCode::kCorrupt);
  EXPECT_EQ(restored.state(), executor.state());
  broken = restored.state();
  broken.local_results[broken.local_head].request.cancel_request_id = {99};
  EXPECT_EQ(restored.RestoreState(broken), ErrorCode::kCorrupt);
  EXPECT_EQ(restored.state(), executor.state());
  broken = restored.state();
  broken.executions[0].cancel_request.state =
      static_cast<OrderRequestState>(99);
  EXPECT_EQ(restored.RestoreState(broken), ErrorCode::kCorrupt);
  EXPECT_EQ(restored.state(), executor.state());
}

TEST_F(OrderRequestExecutorTest,
       RestoreCrossChecksCorePendingCancelAndLocalResult) {
  QueueNew(0);
  Send();
  Accept(0);
  Cancel(0, 5);
  std::array<model::OrderSnapshot, 1> orders{Snapshot(0)};
  orders[0].cancel_pending = true;
  orders[0].cancel_request_id = {5};
  OrderRequestExecutor restored;
  ASSERT_EQ(restored.RestoreState(executor.state(), orders), ErrorCode::kOk);
  orders[0].cancel_pending = false;
  EXPECT_EQ(restored.RestoreState(executor.state(), orders),
            ErrorCode::kCorrupt);
  orders[0].cancel_pending = true;
  const auto filled =
      executor.PrepareReport(0, Report(0, model::OrderUpdateKind::kFill),
                             Snapshot(0, model::OrderState::kFilled));
  ASSERT_TRUE(filled.ok());
  executor.Commit(filled);
  orders[0].state = model::OrderState::kFilled;
  ASSERT_EQ(restored.RestoreState(executor.state(), orders), ErrorCode::kOk);
  EXPECT_TRUE(restored.state().executions[0].cancel_local_result_pending);
}

TEST_F(OrderRequestExecutorTest,
       RecoveryClearsSendsAndKeepsRecordedLocalResults) {
  QueueNew(0);
  Send();
  QueueNew(1);
  QueueNew(2);
  const auto local = executor.PrepareCancelBeforeSend(2, 400);
  ASSERT_TRUE(local.ok());
  executor.Commit(local);
  const auto saved_local = executor.local_front();
  executor.AbandonRecoveredRequests();
  EXPECT_FALSE(executor.front());
  EXPECT_FALSE(executor.next_deadline());
  EXPECT_EQ(executor.state().executions[0].new_request.state,
            OrderRequestState::kUncertain);
  EXPECT_EQ(executor.state().executions[1].new_request.state,
            OrderRequestState::kUncertain);
  EXPECT_EQ(executor.local_front(), saved_local);
  EXPECT_FALSE(executor.is_drained());
  EXPECT_EQ(executor.PrepareCancelBeforeSend(1, 500).code,
            ErrorCode::kInvalidState);
  const auto received =
      executor.PrepareReport(1, Report(1, model::OrderUpdateKind::kRejected),
                             Snapshot(1, model::OrderState::kRejected));
  ASSERT_TRUE(received.ok());
  executor.Commit(received);
  EXPECT_EQ(executor.state().executions[1].new_request.state,
            OrderRequestState::kCompleted);
  OrderRequestExecutor restored;
  EXPECT_EQ(restored.RestoreState(executor.state()), ErrorCode::kOk);
}

TEST_F(OrderRequestExecutorTest, RejectedInputsAndDeadlineOverflowAreAtomic) {
  EXPECT_EQ(executor.Configure(0), ErrorCode::kInvalidArgument);
  EXPECT_EQ(executor.PrepareQueueNew(1, {1001}).code,
            ErrorCode::kInvalidArgument);
  QueueNew(0);
  const auto before = executor.state();
  EXPECT_EQ(executor.Configure(100), ErrorCode::kInvalidState);
  EXPECT_EQ(executor.PrepareBeginSend(UINT64_MAX).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(executor.PrepareQueueCancel(0, {1000}, {1}).code,
            ErrorCode::kInvalidState);
  EXPECT_EQ(executor.PrepareUncertain(*executor.front()).code,
            ErrorCode::kInvalidState);
  EXPECT_EQ(executor.state(), before);
}

}  // namespace
}  // namespace yquant::trading
