#include "yquant/trading/order_core.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <limits>
#include <type_traits>

#include "gtest/gtest.h"
#include "yquant/base/status.h"
#include "yquant/model/futures.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;

// Produces an independent unit-constrained request without a market service.
OrderEvent Creation() {
  OrderEvent event;
  event.kind = OrderEventKind::kCreated;
  event.client_order_id = {7};
  event.now = 1000;
  event.request = {{1234}, model::OrderSide::kBuy, 100, 20};
  event.rules = {5, 2, false};
  return event;
}

class OrderCoreTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(core.Apply(Creation()).ok()); }

  // Builds a report with a stable broker identity and transaction time.
  OrderEvent Report(uint64_t sequence, OrderUpdateKind kind,
                    int64_t cumulative = 0) const {
    OrderEvent event;
    event.kind = OrderEventKind::kBrokerReport;
    event.client_order_id = {7};
    event.now = 2000 + sequence;
    event.report.client_order_id = {7};
    event.report.report_sequence = sequence;
    event.report.kind = kind;
    event.report.cumulative_filled_qty = cumulative;
    event.report.receive_time_ns = 1000 + sequence;
    EXPECT_EQ(event.report.adapter_order_id.Assign("broker-7"), ErrorCode::kOk);
    return event;
  }

  // Adds the unique execution that explains the report's cumulative quantity.
  OrderEvent Fill(uint64_t sequence, int64_t quantity,
                  int64_t cumulative) const {
    auto event = Report(sequence, OrderUpdateKind::kFill, cumulative);
    auto& fill = event.report.fill;
    fill.client_order_id = {7};
    fill.adapter_order_id = event.report.adapter_order_id;
    EXPECT_EQ(fill.execution_id.Assign(sequence == 1 ? "first" : "second"),
              ErrorCode::kOk);
    fill.instrument_id = {1234};
    fill.side = model::OrderSide::kBuy;
    fill.price = 95;
    fill.quantity = quantity;
    fill.receive_time_ns = event.report.receive_time_ns;
    return event;
  }

  // Produces a local input for the selected attempt.
  OrderEvent Local(OrderEventKind kind, uint64_t cancel_id = 0) const {
    OrderEvent event;
    event.kind = kind;
    event.client_order_id = {7};
    event.now = 3000;
    event.cancel_request_id = {cancel_id};
    return event;
  }

  // Accepts the order before testing working-order operations.
  void Accept() {
    ASSERT_TRUE(core.Apply(Report(1, OrderUpdateKind::kAccepted)).ok());
  }

  // Confirms rejected input preserves the complete order and emits no actions.
  void RejectWithoutMutation(const OrderEvent& event, ErrorCode expected) {
    const auto old = core.snapshot();
    const auto rules = core.rules();
    const auto result = core.Apply(event);
    EXPECT_EQ(result.code, expected);
    EXPECT_EQ(result.action_count, 0U);
    EXPECT_GT(result.diagnostic.size, 0);
    EXPECT_EQ(core.snapshot(), old);
    EXPECT_EQ(core.rules(), rules);
  }

  OrderCore core;
};

TEST(OrderCore, CreationOwnsRequestAndProducesOneBusinessCommand) {
  auto event = Creation();
  OrderCore core;
  const auto result = core.Apply(event);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 1U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kSubmitOrder);
  EXPECT_EQ(result.request.limit_price, 100);
  EXPECT_EQ(result.request.quantity, 20);
  EXPECT_EQ(core.snapshot().client_order_id.value, 7U);
  EXPECT_EQ(core.snapshot().state, OrderState::kPendingNew);
  EXPECT_EQ(core.snapshot().original_qty, 20);
  EXPECT_EQ(core.snapshot().leaves_qty, 20);
  EXPECT_EQ(core.snapshot().report_sequence, 0U);
  EXPECT_FALSE(core.snapshot().cancel_pending);
  event.request.quantity = 100;
  event.rules.quantity_step = 10;
  EXPECT_EQ(core.snapshot().original_qty, 20);
  EXPECT_EQ(core.rules().quantity_step, 2);
  static_assert(std::is_trivially_copyable_v<OrderCore>);
  static_assert(std::is_trivially_copyable_v<OrderApplyResult>);
}

TEST(OrderCore, InvalidCreationLeavesCoreAvailableForAValidRequest) {
  auto valid = Creation();
  std::array<OrderEvent, 10> invalid;
  invalid.fill(valid);
  invalid[0].client_order_id = {};
  invalid[1].request.instrument_id = {};
  invalid[2].request.side = model::OrderSide::kUnknown;
  invalid[3].request.side = static_cast<model::OrderSide>(255);
  invalid[4].request.position_effect = static_cast<model::PositionEffect>(255);
  invalid[5].request.quantity = 0;
  invalid[6].request.limit_price = 101;
  invalid[7].request.quantity = 19;
  invalid[8].rules.price_tick = 0;
  invalid[9].rules.quantity_step = -1;
  for (const auto& event : invalid) {
    OrderCore core;
    const auto failed = core.Apply(event);
    EXPECT_EQ(failed.code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(failed.action_count, 0U);
    EXPECT_FALSE(core.initialized());
    EXPECT_TRUE(core.Apply(valid).ok());
  }
}

TEST_F(OrderCoreTest, InvalidEventIdentityAndKindPreserveState) {
  auto wrong = Report(1, OrderUpdateKind::kAccepted);
  wrong.client_order_id = {8};
  RejectWithoutMutation(wrong, ErrorCode::kInvalidState);
  wrong = Report(1, OrderUpdateKind::kAccepted);
  wrong.report.client_order_id = {8};
  RejectWithoutMutation(wrong, ErrorCode::kCorrupt);
  auto unknown = Local(static_cast<OrderEventKind>(255));
  RejectWithoutMutation(unknown, ErrorCode::kInvalidArgument);
  RejectWithoutMutation(Creation(), ErrorCode::kInvalidState);
}

TEST_F(OrderCoreTest, CandidateCopiesDoNotPublishTheirStateToTheOriginal) {
  auto candidate = core;
  const auto result = candidate.Apply(Report(1, OrderUpdateKind::kAccepted));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(core.snapshot().state, OrderState::kPendingNew);
  EXPECT_EQ(candidate.snapshot().state, OrderState::kWorking);
  EXPECT_EQ(result.update.order, candidate.snapshot());
  core = candidate;
  EXPECT_EQ(core.snapshot().state, OrderState::kWorking);
}

TEST_F(OrderCoreTest, PartialFillCanBeTheFirstReportAndLateAcceptanceIsSilent) {
  const auto fill = core.Apply(Fill(1, 8, 8));
  ASSERT_TRUE(fill.ok());
  ASSERT_EQ(fill.action_count, 2U);
  EXPECT_EQ(fill.actions[0], OrderActionKind::kApplyExecution);
  EXPECT_EQ(fill.actions[1], OrderActionKind::kPublishOrderUpdate);
  EXPECT_EQ(fill.fill.quantity, 8);
  EXPECT_EQ(core.snapshot().state, OrderState::kWorking);
  EXPECT_EQ(core.snapshot().leaves_qty, 12);
  EXPECT_EQ(core.snapshot().adapter_order_id.view(), "broker-7");
  const auto accepted = core.Apply(Report(2, OrderUpdateKind::kAccepted, 8));
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.outcome, OrderApplyOutcome::kApplied);
  EXPECT_EQ(accepted.action_count, 0U);
  EXPECT_EQ(core.snapshot().report_sequence, 2U);
  EXPECT_EQ(core.snapshot().receive_time_ns, 1002U);
  EXPECT_EQ(core.snapshot().cumulative_filled_qty, 8);
}

TEST_F(OrderCoreTest, FullFillBeforeAcceptanceDoesNotRegressOnAcceptance) {
  ASSERT_TRUE(core.Apply(Fill(1, 20, 20)).ok());
  const auto accepted = core.Apply(Report(2, OrderUpdateKind::kAccepted, 20));
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.action_count, 0U);
  EXPECT_EQ(core.snapshot().state, OrderState::kFilled);
  EXPECT_EQ(core.snapshot().leaves_qty, 0);
  EXPECT_EQ(core.snapshot().report_sequence, 2U);
  RejectWithoutMutation(Fill(3, 2, 20), ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, RejectionReleasesResourcesAndCannotBeReversed) {
  auto event = Report(1, OrderUpdateKind::kRejected);
  event.report.adapter_order_id = {};
  event.report.reason.Set(23, "rejected");
  const auto rejected = core.Apply(event);
  ASSERT_TRUE(rejected.ok());
  ASSERT_EQ(rejected.action_count, 2U);
  EXPECT_EQ(rejected.actions[0], OrderActionKind::kReleaseRemainingReservation);
  EXPECT_EQ(rejected.actions[1], OrderActionKind::kPublishOrderUpdate);
  EXPECT_EQ(rejected.update.reason.view(), "rejected");
  EXPECT_EQ(core.snapshot().state, OrderState::kRejected);
  EXPECT_EQ(core.snapshot().leaves_qty, 0);
  RejectWithoutMutation(Report(2, OrderUpdateKind::kAccepted),
                        ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, CancelRegistrationOnlyChangesTheBusinessAttempt) {
  RejectWithoutMutation(Local(OrderEventKind::kCancelRequested, 41),
                        ErrorCode::kInvalidState);
  Accept();
  RejectWithoutMutation(Local(OrderEventKind::kCancelRequested),
                        ErrorCode::kInvalidArgument);
  const auto result = core.Apply(Local(OrderEventKind::kCancelRequested, 41));
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 1U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kCancelOrder);
  EXPECT_EQ(result.update.cancel_request_id.value, 41U);
  EXPECT_TRUE(core.snapshot().cancel_pending);
  EXPECT_EQ(core.snapshot().state, OrderState::kWorking);
  EXPECT_EQ(core.snapshot().leaves_qty, 20);
  EXPECT_EQ(core.snapshot().report_sequence, 1U);
  RejectWithoutMutation(Local(OrderEventKind::kCancelRequested, 42),
                        ErrorCode::kInvalidState);
}

TEST_F(OrderCoreTest, CancelNotSentSettlesAttemptWithoutAdvancingBrokerTime) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  ASSERT_TRUE(core.Apply(Fill(2, 20, 20)).ok());
  EXPECT_TRUE(core.snapshot().cancel_pending);
  const auto filled = core.snapshot();
  RejectWithoutMutation(Local(OrderEventKind::kCancelNotSent, 42),
                        ErrorCode::kCorrupt);
  auto event = Local(OrderEventKind::kCancelNotSent, 41);
  event.reason.Set(0, "filled before cancel dispatch");
  const auto result = core.Apply(event);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 1U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kPublishOrderUpdate);
  EXPECT_EQ(result.update.kind, OrderUpdateKind::kCancelNotSent);
  EXPECT_EQ(result.update.cancel_request_id.value, 41U);
  EXPECT_EQ(core.snapshot().state, OrderState::kFilled);
  EXPECT_FALSE(core.snapshot().cancel_pending);
  EXPECT_EQ(core.snapshot().cancel_request_id.value, 41U);
  EXPECT_EQ(core.snapshot().report_sequence, filled.report_sequence);
  EXPECT_EQ(core.snapshot().receive_time_ns, filled.receive_time_ns);
  RejectWithoutMutation(event, ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, CancelNotSentCannotEndAWorkingOrderAttempt) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  RejectWithoutMutation(Local(OrderEventKind::kCancelNotSent, 41),
                        ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, InFlightCancelRejectionPreservesTheFilledLifecycle) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  ASSERT_TRUE(core.Apply(Fill(2, 20, 20)).ok());
  auto rejected = Report(3, OrderUpdateKind::kCancelRejected, 20);
  rejected.report.cancel_request_id = {41};
  const auto result = core.Apply(rejected);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 1U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kPublishOrderUpdate);
  EXPECT_EQ(core.snapshot().state, OrderState::kFilled);
  EXPECT_FALSE(core.snapshot().cancel_pending);
  EXPECT_EQ(core.snapshot().leaves_qty, 0);
}

TEST_F(OrderCoreTest, CancelSuccessCannotReverseAFullFill) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  ASSERT_TRUE(core.Apply(Fill(2, 20, 20)).ok());
  auto canceled = Report(3, OrderUpdateKind::kCanceled, 20);
  canceled.report.cancel_request_id = {41};
  RejectWithoutMutation(canceled, ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, PartialFillThenCancellationPreservesExecutedQuantity) {
  ASSERT_TRUE(core.Apply(Fill(1, 8, 8)).ok());
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  auto canceled = Report(2, OrderUpdateKind::kCanceled, 8);
  canceled.report.cancel_request_id = {41};
  const auto result = core.Apply(canceled);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 2U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kReleaseRemainingReservation);
  EXPECT_EQ(core.snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(core.snapshot().cumulative_filled_qty, 8);
  EXPECT_EQ(core.snapshot().leaves_qty, 0);
  const auto accepted = core.Apply(Report(3, OrderUpdateKind::kAccepted, 8));
  EXPECT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.action_count, 0U);
  RejectWithoutMutation(Fill(4, 12, 20), ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, OldCancelResultCannotClearANewerAttempt) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  auto rejected = Report(2, OrderUpdateKind::kCancelRejected);
  rejected.report.cancel_request_id = {41};
  ASSERT_TRUE(core.Apply(rejected).ok());
  RejectWithoutMutation(Local(OrderEventKind::kCancelRequested, 41),
                        ErrorCode::kInvalidArgument);
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 42)).ok());
  const auto duplicate = core.Apply(rejected);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_EQ(duplicate.outcome, OrderApplyOutcome::kIgnored);
  EXPECT_EQ(duplicate.action_count, 0U);
  EXPECT_TRUE(core.snapshot().cancel_pending);
  rejected.report.report_sequence = 3;
  RejectWithoutMutation(rejected, ErrorCode::kCorrupt);
  rejected.report.cancel_request_id = {42};
  ASSERT_TRUE(core.Apply(rejected).ok());
  EXPECT_FALSE(core.snapshot().cancel_pending);
}

TEST_F(OrderCoreTest, LocalNewCancellationNeedsNoSendStateAndCannotBeReversed) {
  auto event = Local(OrderEventKind::kCanceledBeforeSend);
  event.reason.Set(0, "stopping before send");
  const auto result = core.Apply(event);
  ASSERT_TRUE(result.ok());
  ASSERT_EQ(result.action_count, 2U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kReleaseRemainingReservation);
  EXPECT_EQ(result.actions[1], OrderActionKind::kPublishOrderUpdate);
  EXPECT_EQ(result.update.kind, OrderUpdateKind::kCanceledBeforeSend);
  EXPECT_EQ(core.snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(core.snapshot().report_sequence, 0U);
  EXPECT_EQ(core.snapshot().receive_time_ns, 0U);
  RejectWithoutMutation(Report(1, OrderUpdateKind::kAccepted),
                        ErrorCode::kCorrupt);
  RejectWithoutMutation(event, ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, LocalNewCancellationRejectsAcceptedBusinessFacts) {
  Accept();
  RejectWithoutMutation(Local(OrderEventKind::kCanceledBeforeSend),
                        ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, UnsolicitedCancelRequiresAnExplicitRule) {
  const auto canceled = Report(1, OrderUpdateKind::kCanceled);
  RejectWithoutMutation(canceled, ErrorCode::kCorrupt);
  auto creation = Creation();
  creation.rules.allow_unsolicited_cancel = true;
  OrderCore broker;
  ASSERT_TRUE(broker.Apply(creation).ok());
  const auto result = broker.Apply(canceled);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(broker.snapshot().state, OrderState::kCanceled);
  EXPECT_EQ(broker.snapshot().adapter_order_id.view(), "broker-7");
  EXPECT_EQ(result.update.cancel_request_id.value, 0U);
  ASSERT_EQ(result.action_count, 2U);
  EXPECT_EQ(result.actions[0], OrderActionKind::kReleaseRemainingReservation);
}

TEST_F(OrderCoreTest, UnsolicitedCancelCannotBypassAPendingLocalAttempt) {
  auto creation = Creation();
  creation.rules.allow_unsolicited_cancel = true;
  core = OrderCore();
  ASSERT_TRUE(core.Apply(creation).ok());
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  RejectWithoutMutation(Report(2, OrderUpdateKind::kCanceled),
                        ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, OldFillIgnoresLifecycleButStillValidatesFields) {
  const auto first = Fill(1, 8, 8);
  ASSERT_TRUE(core.Apply(first).ok());
  ASSERT_TRUE(core.Apply(Fill(2, 12, 20)).ok());
  const auto filled = core.snapshot();
  const auto duplicate = core.Apply(first);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_EQ(duplicate.outcome, OrderApplyOutcome::kIgnored);
  EXPECT_EQ(duplicate.action_count, 0U);
  EXPECT_EQ(core.snapshot(), filled);
  auto malformed = first;
  malformed.report.fill.side = model::OrderSide::kSell;
  RejectWithoutMutation(malformed, ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, MalformedFieldsAndSequenceGapsPreserveTheOrder) {
  Accept();
  const auto valid = Fill(2, 8, 8);
  std::array<OrderEvent, 11> invalid;
  invalid.fill(valid);
  invalid[0].report.report_sequence = 3;
  invalid[1].report.cumulative_filled_qty = 10;
  invalid[2].report.fill.price = 105;
  invalid[3].report.fill.price = 96;
  invalid[4].report.fill.quantity = 3;
  invalid[5].report.receive_time_ns = valid.now + 1;
  invalid[6].report.reason.size = 255;
  invalid[7].report.reason.text[0] = 'x';
  invalid[8].report.fill.receive_time_ns = 0;
  invalid[9].report.fill.event_time_ns = 1;
  EXPECT_EQ(invalid[10].report.adapter_order_id.Assign("different"),
            ErrorCode::kOk);
  invalid[10].report.fill.adapter_order_id =
      invalid[10].report.adapter_order_id;
  for (const auto& event : invalid) {
    RejectWithoutMutation(event, ErrorCode::kCorrupt);
  }
}

TEST_F(OrderCoreTest, NonFillPayloadAndLocalKindsAreRejectedAtBrokerBoundary) {
  auto accepted = Report(1, OrderUpdateKind::kAccepted);
  accepted.report.fill.quantity = 2;
  RejectWithoutMutation(accepted, ErrorCode::kCorrupt);
  RejectWithoutMutation(Report(1, OrderUpdateKind::kCanceledBeforeSend),
                        ErrorCode::kCorrupt);
  RejectWithoutMutation(Report(1, OrderUpdateKind::kCancelNotSent),
                        ErrorCode::kCorrupt);
  RejectWithoutMutation(Report(1, static_cast<OrderUpdateKind>(255)),
                        ErrorCode::kCorrupt);
  Accept();
  RejectWithoutMutation(Report(2, OrderUpdateKind::kAccepted, 2),
                        ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, RestorePreservesPendingAttemptAndProducesNoCommands) {
  Accept();
  ASSERT_TRUE(core.Apply(Local(OrderEventKind::kCancelRequested, 41)).ok());
  ASSERT_TRUE(core.Apply(Fill(2, 20, 20)).ok());
  const auto restored = OrderCore::Restore(core.snapshot(), core.rules());
  ASSERT_TRUE(restored.ok());
  EXPECT_TRUE(restored.core.initialized());
  EXPECT_EQ(restored.core.snapshot(), core.snapshot());
  auto copy = restored.core;
  ASSERT_TRUE(copy.Apply(Local(OrderEventKind::kCancelNotSent, 41)).ok());
  EXPECT_FALSE(copy.snapshot().cancel_pending);
  EXPECT_TRUE(restored.core.snapshot().cancel_pending);
}

TEST_F(OrderCoreTest, RestoreRejectsInconsistentSnapshotsAndMalformedRules) {
  Accept();
  const auto valid = core.snapshot();
  std::array<model::OrderSnapshot, 8> invalid;
  invalid.fill(valid);
  invalid[0].state = static_cast<OrderState>(255);
  invalid[1].strategy_order_id = {3};
  invalid[2].leaves_qty = 19;
  invalid[3].cumulative_filled_qty = 1;
  invalid[4].cancel_pending = true;
  invalid[5].report_sequence = 0;
  invalid[6].adapter_order_id = {};
  invalid[7].state = OrderState::kFilled;
  for (const auto& snapshot : invalid) {
    const auto result = OrderCore::Restore(snapshot, core.rules());
    EXPECT_EQ(result.code, ErrorCode::kCorrupt);
    EXPECT_FALSE(result.core.initialized());
    EXPECT_GT(result.diagnostic.size, 0);
  }
  auto rules = core.rules();
  rules.quantity_step = 0;
  EXPECT_EQ(OrderCore::Restore(valid, rules).code, ErrorCode::kCorrupt);
}

TEST_F(OrderCoreTest, RestoredAndContinuousCoresProduceTheSameSuffix) {
  ASSERT_TRUE(core.Apply(Fill(1, 8, 8)).ok());
  auto restored = OrderCore::Restore(core.snapshot(), core.rules());
  ASSERT_TRUE(restored.ok());
  const std::array events = {Report(2, OrderUpdateKind::kAccepted, 8),
                             Local(OrderEventKind::kCancelRequested, 41),
                             Fill(3, 12, 20),
                             Local(OrderEventKind::kCancelNotSent, 41)};
  for (const auto& event : events) {
    const auto live = core.Apply(event);
    const auto replay = restored.core.Apply(event);
    ASSERT_TRUE(live.ok());
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(core.snapshot(), restored.core.snapshot());
    EXPECT_EQ(live.outcome, replay.outcome);
    EXPECT_EQ(live.action_count, replay.action_count);
    EXPECT_EQ(live.actions, replay.actions);
    EXPECT_EQ(live.update.order, replay.update.order);
    EXPECT_EQ(live.update.kind, replay.update.kind);
  }
}

TEST(OrderCore, MaximumQuantityDoesNotOverflowIncrementValidation) {
  auto creation = Creation();
  creation.rules.quantity_step = 1;
  creation.request.quantity = std::numeric_limits<int64_t>::max();
  OrderCore core;
  ASSERT_TRUE(core.Apply(creation).ok());
  OrderEvent event;
  event.kind = OrderEventKind::kBrokerReport;
  event.client_order_id = {7};
  event.now = 1;
  auto& report = event.report;
  report.client_order_id = {7};
  report.kind = OrderUpdateKind::kFill;
  report.report_sequence = 1;
  ASSERT_EQ(report.adapter_order_id.Assign("broker"), ErrorCode::kOk);
  report.fill.client_order_id = {7};
  report.fill.adapter_order_id = report.adapter_order_id;
  ASSERT_EQ(report.fill.execution_id.Assign("execution"), ErrorCode::kOk);
  report.fill.instrument_id = {1234};
  report.fill.side = model::OrderSide::kBuy;
  report.fill.price = 100;
  report.fill.quantity = std::numeric_limits<int64_t>::max() - 1;
  report.cumulative_filled_qty = report.fill.quantity;
  ASSERT_TRUE(core.Apply(event).ok());
  const auto old = core.snapshot();
  report.report_sequence = 2;
  report.fill.quantity = 2;
  report.cumulative_filled_qty = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(core.Apply(event).code, ErrorCode::kCorrupt);
  EXPECT_EQ(core.snapshot(), old);
}

}  // namespace
}  // namespace yquant::trading
