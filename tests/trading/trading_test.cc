#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "yquant/market/instrument_scope.h"
#include "yquant/model/identifiers.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;

class LedgerClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000;
};

// Records sends without generating callbacks; tests explicitly drive reports.
class RecordingTradingAdapter final : public trading::TradingAdapter {
 public:
  base::Status Prepare(const trading::TradingConfig& config) noexcept override {
    account = config.initial_account;
    return {};
  }
  base::Status Start(uint64_t) noexcept override { return {}; }
  base::Status ReadAccountSnapshot(
      trading::TradingAccountSnapshot& out) noexcept override {
    out = account;
    return {};
  }
  base::Status SendOrder(model::ClientOrderId id,
                         const model::OrderRequest& request,
                         uint64_t) noexcept override {
    ++order_sends;
    last_order_id = id;
    last_order = request;
    return {fail_send ? ErrorCode::kIoError : ErrorCode::kOk, {}};
  }
  base::Status SendCancel(model::ClientOrderId id,
                          const model::AdapterOrderId& broker,
                          model::CancelRequestId cancel,
                          uint64_t) noexcept override {
    ++cancel_sends;
    last_order_id = id;
    last_broker = broker;
    last_cancel_id = cancel;
    return {fail_send ? ErrorCode::kIoError : ErrorCode::kOk, {}};
  }
  trading::TradingPollResult Poll(uint64_t,
                                  model::TradingEvent& out) noexcept override {
    if (pending_event) {
      out = *pending_event;
      pending_event.reset();
      return {trading::TradingPollState::kEvent, {}};
    }
    return {};
  }
  std::optional<uint64_t> next_deadline() const noexcept override {
    return pending_event ? std::optional<uint64_t>{0} : std::nullopt;
  }
  bool is_drained() const noexcept override { return !pending_event; }
  base::Status Stop() noexcept override { return {}; }
  const char* error_detail() const noexcept override {
    return "injected send failure";
  }
  model::ClientOrderId last_order_id;
  model::CancelRequestId last_cancel_id;
  model::AdapterOrderId last_broker;
  model::OrderRequest last_order;
  size_t order_sends = 0;
  size_t cancel_sends = 0;
  bool fail_send = false;
  trading::TradingAccountSnapshot account;
  std::optional<model::TradingEvent> pending_event;
};

class TradingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& config = market_config;
    config.source_id = {1};

    config.instrument_count = 1;
    auto& item = config.instruments[0];
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
              ErrorCode::kOk);
    instrument_id = item.instrument.id;
    item.instrument.venue = model::Venue::kSse;
    std::memcpy(item.instrument.symbol, "600000", model::kSymbolLength);
    item.instrument.price_decimals = 2;

    ASSERT_TRUE(market_data.Configure(config).ok());
    trading::TradingAccountSnapshot account;
    account.cash_balance = std::numeric_limits<int64_t>::max();
    ASSERT_TRUE(InitializeAccount(account).ok());
  }

  base::Status InitializeAccount(
      const trading::TradingAccountSnapshot& account) {
    trading::TradingConfig config;
    config.initial_account = account;
    return trading.Init(config, market_config);
  }

  model::OrderRequest Request(int64_t quantity = 200) const {
    return {instrument_id, model::OrderSide::kBuy, 1001, quantity};
  }
  model::ClientOrderId Submit(int64_t quantity = 200) {
    model::ClientOrderId id;
    trading.SetAcceptingRequests(true);
    EXPECT_EQ(trading.SubmitOrder(Request(quantity), id), ErrorCode::kOk);
    trading.SetAcceptingRequests(false);
    return id;
  }
  model::OrderSnapshot Snapshot(model::ClientOrderId id) const {
    model::OrderSnapshot result;
    EXPECT_TRUE(trading.TryGetOrder(id, result));
    return result;
  }
  void Send() { ASSERT_TRUE(trading.ProcessRequests(clock.now).ok()); }
  void Bootstrap(int64_t cash, int64_t total = 0, int64_t available = 0,
                 bool include_position = false) {
    trading.Reset();
    trading::TradingAccountSnapshot account;
    account.cash_balance = cash;
    if (include_position) {
      account.position_count = 1;
      account.positions[0] = {instrument_id, total, available, 0};
    }
    ASSERT_TRUE(InitializeAccount(account).ok());
  }
  void Precision(uint8_t decimals) {
    trading.Reset();
    market_data.Reset();
    market_config.instruments[0].instrument.price_decimals = decimals;
    ASSERT_TRUE(market_data.Configure(market_config).ok());
  }
  model::CashSnapshot Cash() const {
    model::CashSnapshot result;
    EXPECT_TRUE(trading.TryGetCash(result));
    return result;
  }
  model::PositionSnapshot Position() const {
    model::PositionSnapshot result;
    EXPECT_TRUE(trading.TryGetPosition(instrument_id, result));
    return result;
  }
  model::ClientOrderId SubmitRequest(const model::OrderRequest& request) {
    model::ClientOrderId id;
    trading.SetAcceptingRequests(true);
    EXPECT_EQ(trading.SubmitOrder(request, id), ErrorCode::kOk);
    trading.SetAcceptingRequests(false);
    return id;
  }
  model::TradingEvent Event(model::ClientOrderId id, uint64_t sequence,
                            OrderUpdateKind kind,
                            int64_t cumulative = 0) const {
    model::TradingEvent event;
    event.client_order_id = id;
    EXPECT_EQ(event.adapter_order_id.Assign("broker-order-" +
                                            std::to_string(id.value)),
              ErrorCode::kOk);
    event.report_sequence = sequence;
    event.kind = kind;
    event.cumulative_filled_qty = cumulative;
    event.receive_time_ns = clock.now;
    return event;
  }
  model::TradingEvent Execution(model::ClientOrderId id, uint64_t sequence,
                                std::string_view execution_id, int64_t quantity,
                                int64_t cumulative) const {
    auto event = Event(id, sequence, OrderUpdateKind::kFill, cumulative);
    event.fill.client_order_id = id;
    event.fill.adapter_order_id = event.adapter_order_id;
    EXPECT_EQ(event.fill.execution_id.Assign(execution_id), ErrorCode::kOk);
    event.fill.instrument_id = instrument_id;
    event.fill.side = Snapshot(id).side;
    event.fill.price = 1001;
    event.fill.quantity = quantity;
    event.fill.receive_time_ns = clock.now;
    return event;
  }
  void Accept(model::ClientOrderId id) {
    Send();
    trading::TradingDispatch dispatch;
    ASSERT_EQ(
        trading.ApplyEvent(Event(id, 1, OrderUpdateKind::kAccepted), dispatch),
        ErrorCode::kOk);
    EXPECT_TRUE(dispatch.notify_order);
    EXPECT_FALSE(dispatch.has_fill);
  }
  void Cancel(model::ClientOrderId id) {
    trading.SetAcceptingRequests(true);
    EXPECT_EQ(trading.CancelOrder(id), ErrorCode::kOk);
    trading.SetAcceptingRequests(false);
  }

  LedgerClock clock;
  market::MarketDataConfig market_config;
  market::InstrumentScope market_data;
  RecordingTradingAdapter adapter;
  trading::TradingEngine trading{&clock, &market_data, &adapter};
  model::InstrumentId instrument_id;
};

TEST_F(TradingTest, HostControlsRequestsAndStoppingCannotBeReopened) {
  model::ClientOrderId id{99};
  EXPECT_FALSE(trading.accepting_orders());
  EXPECT_EQ(trading.SubmitOrder(Request(), id), ErrorCode::kInvalidState);
  EXPECT_EQ(id.value, 99U);
  trading.SetAcceptingRequests(true);
  ASSERT_TRUE(trading.accepting_orders());
  ASSERT_EQ(trading.SubmitOrder(Request(), id), ErrorCode::kOk);
  const auto before = Snapshot(id);
  EXPECT_EQ(trading.ProcessRequests(clock.now).code, ErrorCode::kInvalidState);
  EXPECT_EQ(adapter.order_sends, 0U);
  trading::TradingDispatch dispatch;
  dispatch.update.order.client_order_id = {99};
  bool event = true;
  EXPECT_EQ(trading.Poll(clock.now, dispatch, event).code,
            ErrorCode::kInvalidState);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 99U);
  EXPECT_TRUE(event);
  EXPECT_EQ(Snapshot(id), before);
  trading.SetAcceptingRequests(false);
  ASSERT_TRUE(trading.ProcessRequests(clock.now).ok());
  EXPECT_EQ(adapter.order_sends, 1U);
  trading.BeginStopping();
  trading.SetAcceptingRequests(true);
  EXPECT_FALSE(trading.accepting_orders());
  model::ClientOrderId unchanged{99};
  EXPECT_EQ(trading.SubmitOrder(Request(), unchanged),
            ErrorCode::kInvalidState);
  EXPECT_EQ(unchanged.value, 99U);
  EXPECT_EQ(trading.CancelOrder(id), ErrorCode::kInvalidState);
  EXPECT_EQ(trading.order_count(), 1U);
}

TEST_F(TradingTest, PollCommitsBeforeReturningAndIdlePreservesDispatch) {
  const auto id = Submit();
  Send();
  adapter.pending_event = Event(id, 1, OrderUpdateKind::kAccepted);
  EXPECT_TRUE(trading.has_ready_event(clock.now));
  trading::TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(trading.Poll(clock.now, dispatch, event).ok());
  EXPECT_TRUE(event);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_EQ(dispatch.update.order, Snapshot(id));
  EXPECT_EQ(Snapshot(id).state, OrderState::kWorking);
  const auto committed = dispatch.update.order;
  ASSERT_TRUE(trading.Poll(clock.now, dispatch, event).ok());
  EXPECT_FALSE(event);
  EXPECT_EQ(dispatch.update.order, committed);
  EXPECT_FALSE(trading.has_ready_event(clock.now));
  EXPECT_FALSE(trading.next_deadline().has_value());
}

TEST_F(TradingTest, PollFailureRetainsReportIdentityReasonAndOutputs) {
  const auto id = Submit();
  Send();
  const auto before = Snapshot(id);
  adapter.pending_event = Event(id, 2, OrderUpdateKind::kAccepted);
  trading::TradingDispatch dispatch;
  dispatch.update.order.client_order_id = {99};
  bool event = true;
  EXPECT_EQ(trading.Poll(clock.now, dispatch, event).code, ErrorCode::kCorrupt);
  EXPECT_STREQ(trading.error_stage(), "trading_event");
  const std::string detail = trading.error_detail();
  EXPECT_NE(detail.find("client_order_id=1"), std::string::npos);
  EXPECT_NE(detail.find("sequence=2"), std::string::npos);
  EXPECT_NE(detail.find("report sequence or broker identity mismatch"),
            std::string::npos);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 99U);
  EXPECT_TRUE(event);
  EXPECT_EQ(Snapshot(id), before);
}

TEST_F(TradingTest, StopRetainsAccountFactsAndResetRequiresNewInitialization) {
  const auto id = Submit();
  const auto cash = Cash();
  const auto order = Snapshot(id);
  ASSERT_TRUE(trading.Stop().ok());
  trading.SetAcceptingRequests(true);
  EXPECT_FALSE(trading.accepting_orders());
  trading.SetAcceptingRequests(false);
  EXPECT_EQ(trading.ProcessRequests(clock.now).code, ErrorCode::kInvalidState);
  EXPECT_EQ(adapter.order_sends, 0U);
  EXPECT_EQ(Cash(), cash);
  EXPECT_EQ(Snapshot(id), order);
  trading.Reset();
  model::CashSnapshot out{7, 0, 7};
  EXPECT_FALSE(trading.TryGetCash(out));
  EXPECT_EQ(out.balance, 7);
  Bootstrap(1000000000);
  EXPECT_FALSE(trading.accepting_orders());
  EXPECT_EQ(trading.order_count(), 0U);
}

TEST(TradingModelTest, OpaqueIdentityIsOwnedAndSupportsFullCapacity) {
  model::AdapterOrderId broker;
  std::string original(64, 'x');
  EXPECT_EQ(broker.Assign(original), ErrorCode::kOk);
  original.assign(64, 'y');
  EXPECT_EQ(broker.view(), std::string(64, 'x'));
  EXPECT_EQ(broker.Assign(std::string(65, 'z')), ErrorCode::kInvalidArgument);
  EXPECT_EQ(broker.view(), std::string(64, 'x'));
  EXPECT_EQ(broker.Assign({}), ErrorCode::kInvalidArgument);
  EXPECT_EQ(broker.view().size(), 64U);
  model::ExecutionId execution;
  ASSERT_EQ(execution.Assign(std::string_view("a\0b", 3)), ErrorCode::kOk);
  EXPECT_EQ(execution.view().size(), 3U);
  auto copied = execution;
  ASSERT_EQ(execution.Assign("other"), ErrorCode::kOk);
  EXPECT_EQ(copied.view(), std::string_view("a\0b", 3));
}

TEST(TradingModelTest, ReasonRetains128BytesAndMarksTruncation) {
  model::OrderReason reason;
  reason.Set(7, std::string(128, 'x'));
  EXPECT_EQ(reason.code, 7);
  EXPECT_EQ(reason.view().size(), 128U);
  EXPECT_FALSE(reason.truncated);
  EXPECT_EQ(reason.text[128], '\0');
  reason.Set(8, std::string(129, 'y'));
  EXPECT_TRUE(reason.truncated);
  EXPECT_EQ(reason.view(), std::string(128, 'y'));
  reason.size = 255;
  EXPECT_EQ(reason.view().size(), 128U);
}

TEST_F(TradingTest,
       LocalAcceptanceImmediatelyQueryableWithoutBrokerAcceptance) {
  const auto id = Submit();
  EXPECT_EQ(adapter.order_sends, 0U);
  EXPECT_EQ(Snapshot(id).state, OrderState::kPendingNew);
  EXPECT_EQ(Snapshot(id).leaves_qty, 200);
  EXPECT_TRUE(Snapshot(id).adapter_order_id.empty());
  EXPECT_EQ(Snapshot(id).report_sequence, 0U);
  EXPECT_FALSE(trading.is_drained());
  Send();
  EXPECT_EQ(adapter.order_sends, 1U);
  EXPECT_EQ(Snapshot(id).state, OrderState::kPendingNew);
  trading::TradingDispatch dispatch;
  ASSERT_EQ(
      trading.ApplyEvent(Event(id, 1, OrderUpdateKind::kAccepted), dispatch),
      ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kWorking);
  EXPECT_EQ(Snapshot(id), dispatch.update.order);
}

TEST_F(TradingTest,
       RequestPermissionDisabledAndInvalidRequestsPreserveOutputs) {
  model::ClientOrderId output{99};
  trading.SetAcceptingRequests(false);
  EXPECT_FALSE(trading.accepting_orders());
  EXPECT_EQ(trading.SubmitOrder(Request(), output), ErrorCode::kInvalidState);
  trading.SetAcceptingRequests(false);
  RecordingTradingAdapter uninitialized_adapter;
  trading::TradingEngine disabled(&clock, &market_data, &uninitialized_adapter);
  disabled.SetAcceptingRequests(true);
  EXPECT_EQ(disabled.SubmitOrder(Request(), output), ErrorCode::kInvalidState);
  EXPECT_EQ(output.value, 99U);
  trading.SetAcceptingRequests(true);
  EXPECT_TRUE(trading.accepting_orders());
  auto request = Request();
  request.quantity = 0;
  EXPECT_EQ(trading.SubmitOrder(request, output), ErrorCode::kInvalidArgument);
  request = Request();
  request.instrument_id = {};
  EXPECT_EQ(trading.SubmitOrder(request, output), ErrorCode::kInvalidArgument);
  request = Request();
  request.side = model::OrderSide::kUnknown;
  EXPECT_EQ(trading.SubmitOrder(request, output), ErrorCode::kInvalidArgument);
  EXPECT_EQ(output.value, 99U);
  EXPECT_EQ(trading.order_count(), 0U);
  trading.SetAcceptingRequests(false);
  auto unchanged = Snapshot(Submit());
  const auto saved = unchanged;
  EXPECT_FALSE(trading.TryGetOrder({999}, unchanged));
  EXPECT_EQ(unchanged, saved);
}

TEST_F(TradingTest, UniqueFillsAdvanceOnceAndOldReportsDoNotNotify) {
  const auto id = Submit();
  Accept(id);
  const auto first = Execution(id, 2, "fill-1", 100, 100);
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(first, dispatch), ErrorCode::kOk);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_TRUE(dispatch.has_fill);
  EXPECT_EQ(Snapshot(id).cumulative_filled_qty, 100);
  EXPECT_EQ(Snapshot(id).leaves_qty, 100);
  const auto once_cash = Cash();
  const auto once_position = Position();
  ASSERT_EQ(trading.ApplyEvent(first, dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.notify_order);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_EQ(Snapshot(id).cumulative_filled_qty, 100);
  EXPECT_EQ(Cash(), once_cash);
  EXPECT_EQ(Position(), once_position);
  const auto before = Snapshot(id);
  dispatch.notify_order = true;
  dispatch.update.order.leaves_qty = 123;
  EXPECT_EQ(trading.ApplyEvent(Execution(id, 3, "fill-1", 1, 101), dispatch),
            ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), before);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_EQ(dispatch.update.order.leaves_qty, 123);
  EXPECT_EQ(Cash(), once_cash);
  EXPECT_EQ(Position(), once_position);
  ASSERT_EQ(trading.ApplyEvent(Execution(id, 3, "fill-2", 100, 200), dispatch),
            ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kFilled);
  EXPECT_EQ(Snapshot(id).leaves_qty, 0);
  EXPECT_TRUE(trading.is_drained());
}

TEST_F(TradingTest,
       FillCanPrecedeAcceptanceAndLateAcceptanceOnlyAdvancesSequence) {
  const auto id = Submit();
  Send();
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(Execution(id, 1, "fill", 200, 200), dispatch),
            ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kFilled);
  ++clock.now;
  ASSERT_EQ(trading.ApplyEvent(Event(id, 2, OrderUpdateKind::kAccepted, 200),
                               dispatch),
            ErrorCode::kOk);
  EXPECT_FALSE(dispatch.notify_order);
  EXPECT_FALSE(dispatch.has_fill);
  EXPECT_EQ(Snapshot(id).state, OrderState::kFilled);
  EXPECT_EQ(Snapshot(id).report_sequence, 2U);
  EXPECT_EQ(Snapshot(id).receive_time_ns, clock.now);
}

TEST_F(TradingTest, InvalidReportsPreserveLedgerAndDispatch) {
  const auto id = Submit();
  Accept(id);
  const auto before = Snapshot(id);
  const auto before_cash = Cash();
  const auto before_position = Position();
  auto invalid = Execution(id, 3, "gap", 1, 1);
  trading::TradingDispatch dispatch;
  dispatch.notify_order = true;
  EXPECT_EQ(trading.ApplyEvent(invalid, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), before);
  EXPECT_TRUE(dispatch.notify_order);
  invalid = Execution(id, 2, "wrong-cumulative", 1, 2);
  EXPECT_EQ(trading.ApplyEvent(invalid, dispatch), ErrorCode::kCorrupt);
  invalid = Execution(id, 2, "wrong-price", 1, 1);
  invalid.fill.price = 1002;
  EXPECT_EQ(trading.ApplyEvent(invalid, dispatch), ErrorCode::kCorrupt);
  invalid = Event(id, 2, OrderUpdateKind::kAccepted);
  invalid.reason.size = 255;
  EXPECT_EQ(trading.ApplyEvent(invalid, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), before);
  EXPECT_EQ(Cash(), before_cash);
  EXPECT_EQ(Position(), before_position);
}

TEST_F(TradingTest, CancelQueuedBeforeFullFillStillSendsAndSettles) {
  const auto id = Submit();
  Accept(id);
  Cancel(id);
  EXPECT_EQ(Snapshot(id).state, OrderState::kWorking);
  EXPECT_EQ(Snapshot(id).leaves_qty, 200);
  EXPECT_TRUE(Snapshot(id).cancel_pending);
  const auto cancel = Snapshot(id).cancel_request_id;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(Execution(id, 2, "full", 200, 200), dispatch),
            ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kFilled);
  EXPECT_TRUE(Snapshot(id).cancel_pending);
  EXPECT_FALSE(trading.is_drained());
  Send();
  EXPECT_EQ(adapter.cancel_sends, 1U);
  EXPECT_EQ(adapter.last_cancel_id, cancel);
  auto rejection = Event(id, 3, OrderUpdateKind::kCancelRejected, 200);
  rejection.cancel_request_id = cancel;
  ASSERT_EQ(trading.ApplyEvent(rejection, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kFilled);
  EXPECT_FALSE(Snapshot(id).cancel_pending);
  EXPECT_TRUE(trading.is_drained());
}

TEST_F(TradingTest, PartialFillThenCancelPreservesExecutedQuantity) {
  const auto id = Submit();
  Accept(id);
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(Execution(id, 2, "part", 100, 100), dispatch),
            ErrorCode::kOk);
  Cancel(id);
  Send();
  auto canceled = Event(id, 3, OrderUpdateKind::kCanceled, 100);
  canceled.cancel_request_id = Snapshot(id).cancel_request_id;
  ASSERT_EQ(trading.ApplyEvent(canceled, dispatch), ErrorCode::kOk);
  const auto terminal = Snapshot(id);
  EXPECT_EQ(terminal.state, OrderState::kCanceled);
  EXPECT_EQ(terminal.cumulative_filled_qty, 100);
  EXPECT_EQ(terminal.leaves_qty, 0);
  EXPECT_TRUE(trading.is_drained());
  EXPECT_EQ(trading.ApplyEvent(Execution(id, 4, "late", 100, 200), dispatch),
            ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), terminal);
}

TEST_F(TradingTest, BrokerIdentityCannotBindTwoClientOrders) {
  const auto first = Submit();
  Accept(first);
  const auto second = Submit();
  Send();
  const auto before = Snapshot(second);
  trading::TradingDispatch dispatch;
  auto duplicate = Event(second, 1, OrderUpdateKind::kAccepted);
  duplicate.adapter_order_id = Snapshot(first).adapter_order_id;
  EXPECT_EQ(trading.ApplyEvent(duplicate, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(second), before);
  EXPECT_EQ(Snapshot(first).state, OrderState::kWorking);
}

TEST_F(TradingTest, NonFillEventRejectsExecutionPayload) {
  const auto id = Submit();
  Accept(id);
  auto event = Event(id, 2, OrderUpdateKind::kAccepted);
  event.fill.quantity = 1;
  const auto before = Snapshot(id);
  trading::TradingDispatch dispatch;
  EXPECT_EQ(trading.ApplyEvent(event, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), before);
}

TEST_F(TradingTest, CancelFailureCannotSettleAnotherAttemptAndRetryWaits) {
  const auto id = Submit();
  Accept(id);
  trading.BeginStopping();
  ASSERT_TRUE(trading.ScheduleStopping(clock.now).ok());
  Send();
  const auto first_id = Snapshot(id).cancel_request_id;
  auto rejected = Event(id, 2, OrderUpdateKind::kCancelRejected);
  rejected.cancel_request_id = first_id;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(rejected, dispatch), ErrorCode::kOk);
  ASSERT_TRUE(trading.ScheduleStopping(clock.now).ok());
  ASSERT_TRUE(trading.next_deadline().has_value());
  EXPECT_EQ(*trading.next_deadline(), clock.now + 10000000);
  clock.now += 9999999;
  ASSERT_TRUE(trading.ScheduleStopping(clock.now).ok());
  Send();
  EXPECT_EQ(adapter.cancel_sends, 1U);
  ++clock.now;
  ASSERT_TRUE(trading.ScheduleStopping(clock.now).ok());
  Send();
  const auto second_id = Snapshot(id).cancel_request_id;
  EXPECT_NE(second_id, first_id);
  const auto before_cash = Cash();
  const auto before_position = Position();
  ASSERT_EQ(trading.ApplyEvent(rejected, dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.notify_order);
  EXPECT_TRUE(Snapshot(id).cancel_pending);
  rejected.report_sequence = 3;
  rejected.receive_time_ns = clock.now;
  EXPECT_EQ(trading.ApplyEvent(rejected, dispatch), ErrorCode::kCorrupt);
  EXPECT_TRUE(Snapshot(id).cancel_pending);
  EXPECT_EQ(Cash(), before_cash);
  EXPECT_EQ(Position(), before_position);
  auto canceled = Event(id, 3, OrderUpdateKind::kCanceled);
  canceled.cancel_request_id = second_id;
  ASSERT_EQ(trading.ApplyEvent(canceled, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Snapshot(id).state, OrderState::kCanceled);
  EXPECT_TRUE(trading.is_drained());
}

TEST_F(TradingTest, StopNeverSendsNewOrdersBeyondTheScanBudget) {
  for (size_t i = 0; i < 200; ++i) {
    (void)Submit();
  }
  trading.BeginStopping();
  trading::TradingDispatch dispatch;
  size_t updates = 0;
  while (!trading.is_drained()) {
    ASSERT_TRUE(trading.ScheduleStopping(clock.now).ok());
    ASSERT_TRUE(trading.ProcessRequests(clock.now).ok());
    size_t batch = 0;
    while (trading.has_local_updates() && batch++ < 64) {
      ASSERT_EQ(trading.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
      EXPECT_EQ(dispatch.update.kind, OrderUpdateKind::kCanceledBeforeSend);
      EXPECT_EQ(dispatch.update.order.state, OrderState::kCanceled);
      EXPECT_EQ(dispatch.update.order.report_sequence, 0U);
      ++updates;
    }
    ASSERT_LE(updates, 200U);
  }
  EXPECT_EQ(updates, 200U);
  EXPECT_EQ(adapter.order_sends, 0U);
  EXPECT_EQ(adapter.cancel_sends, 0U);
}

TEST_F(TradingTest, SendFailureIsNotAutomaticallyRetriedOrLocallyCanceled) {
  const auto id = Submit();
  adapter.fail_send = true;
  EXPECT_EQ(trading.ProcessRequests(clock.now).code, ErrorCode::kIoError);
  EXPECT_EQ(adapter.order_sends, 1U);
  EXPECT_TRUE(trading.ProcessRequests(clock.now).ok());
  EXPECT_EQ(adapter.order_sends, 1U);
  trading.BeginStopping();
  EXPECT_TRUE(trading.ScheduleStopping(clock.now).ok());
  EXPECT_TRUE(trading.ProcessRequests(clock.now).ok());
  EXPECT_FALSE(trading.has_local_updates());
  EXPECT_EQ(Snapshot(id).state, OrderState::kPendingNew);
  EXPECT_FALSE(trading.is_drained());
}

TEST_F(TradingTest,
       FixedRequestAndOrderLimitsRejectWithoutPartialRegistration) {
  for (size_t i = 0; i < trading::kMaxTradingRequests; ++i) {
    (void)Submit();
  }
  trading.SetAcceptingRequests(true);
  model::ClientOrderId out{99999};
  const auto request_cash = Cash();
  const auto request_position = Position();
  EXPECT_EQ(trading.SubmitOrder(Request(), out), ErrorCode::kResourceExhausted);
  EXPECT_EQ(out.value, 99999U);
  EXPECT_EQ(Cash(), request_cash);
  EXPECT_EQ(Position(), request_position);
  trading.SetAcceptingRequests(false);
  EXPECT_EQ(trading.order_count(), trading::kMaxTradingRequests);
  ASSERT_TRUE(
      trading.ProcessRequests(clock.now, trading::kMaxTradingRequests).ok());
  for (size_t count = trading::kMaxTradingRequests;
       count < trading::kMaxTradingOrders; ++count) {
    (void)Submit();
    Send();
  }
  trading.SetAcceptingRequests(true);
  const auto order_cash = Cash();
  const auto order_position = Position();
  EXPECT_EQ(trading.SubmitOrder(Request(), out), ErrorCode::kResourceExhausted);
  EXPECT_EQ(out.value, 99999U);
  EXPECT_EQ(trading.order_count(), trading::kMaxTradingOrders);
  EXPECT_EQ(Cash(), order_cash);
  EXPECT_EQ(Position(), order_position);
  trading.SetAcceptingRequests(false);
}

TEST_F(TradingTest, ExecutionCapacityFailurePreservesLastAcceptedFacts) {
  const auto id = Submit(static_cast<int64_t>(trading::kMaxTradingFills + 1));
  Accept(id);
  trading::TradingDispatch dispatch;
  for (size_t count = 0; count < trading::kMaxTradingFills; ++count) {
    ASSERT_EQ(trading.ApplyEvent(Execution(id, count + 2, std::to_string(count),
                                           1, static_cast<int64_t>(count + 1)),
                                 dispatch),
              ErrorCode::kOk);
  }
  const auto before = Snapshot(id);
  const auto before_cash = Cash();
  const auto before_position = Position();
  EXPECT_EQ(trading.ApplyEvent(
                Execution(id, trading::kMaxTradingFills + 2, "overflow", 1,
                          static_cast<int64_t>(trading::kMaxTradingFills + 1)),
                dispatch),
            ErrorCode::kResourceExhausted);
  EXPECT_EQ(Snapshot(id), before);
  EXPECT_EQ(Snapshot(id).leaves_qty, 1);
  EXPECT_EQ(Cash(), before_cash);
  EXPECT_EQ(Position(), before_position);
}
TEST_F(TradingTest, AccountQueriesPreserveOutputsForDisabledAndUnknownIds) {
  RecordingTradingAdapter uninitialized_adapter;
  trading::TradingEngine disabled(&clock, &market_data, &uninitialized_adapter);
  model::CashSnapshot cash{1, 2, 3};
  EXPECT_FALSE(disabled.TryGetCash(cash));
  EXPECT_EQ(cash, (model::CashSnapshot{1, 2, 3}));
  model::PositionSnapshot position{{99}, 4, 3, 1};
  const auto sentinel = position;
  EXPECT_FALSE(disabled.TryGetPosition(instrument_id, position));
  EXPECT_EQ(position, sentinel);
  EXPECT_FALSE(trading.TryGetPosition({99}, position));
  EXPECT_EQ(position, sentinel);
  ASSERT_TRUE(trading.TryGetPosition(instrument_id, position));
  EXPECT_EQ(position, (model::PositionSnapshot{instrument_id, 0, 0, 0}));
  Bootstrap(0, 0, 0, true);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 0, 0, 0}));
  EXPECT_EQ(Cash(), (model::CashSnapshot{0, 0, 0}));
}

TEST_F(TradingTest, InvalidBootstrapNeverPublishesAnAccount) {
  trading::TradingAccountSnapshot good;
  good.cash_balance = 100;
  good.position_count = 1;
  good.positions[0] = {instrument_id, 100, 80, 0};
  for (int scenario = 0; scenario < 8; ++scenario) {
    SCOPED_TRACE(scenario);
    auto invalid = good;
    switch (scenario) {
      case 0:
        invalid.cash_balance = -1;
        break;
      case 1:
        invalid.position_count = market::kMaxMarketDataInstruments + 1;
        break;
      case 2:
        invalid.positions[0].instrument_id = {99};
        break;
      case 3:
        invalid.positions[0].total_qty = -1;
        break;
      case 4:
        invalid.positions[0].available_qty = -1;
        break;
      case 5:
        invalid.positions[0].available_qty = 101;
        break;
      case 6:
        invalid.positions[0].reserved_qty = 1;
        break;
      case 7:
        invalid.positions[1] = invalid.positions[0];
        invalid.position_count = 2;
        break;
    }
    RecordingTradingAdapter bootstrap_adapter;
    trading::TradingEngine runtime(&clock, &market_data, &bootstrap_adapter);
    trading::TradingConfig config;
    config.initial_account = invalid;
    EXPECT_EQ(runtime.Init(config, market_config).code,
              ErrorCode::kInvalidArgument);
    model::CashSnapshot out{7, 0, 7};
    EXPECT_FALSE(runtime.TryGetCash(out));
    EXPECT_EQ(out.balance, 7);
    EXPECT_EQ(runtime.order_count(), 0U);
    runtime.Reset();
    config.initial_account = good;
    ASSERT_TRUE(runtime.Init(config, market_config).ok());
    EXPECT_TRUE(runtime.TryGetCash(out));
    EXPECT_EQ(out, (model::CashSnapshot{100, 0, 100}));
  }
  const auto before = Cash();
  EXPECT_EQ(InitializeAccount(good).code, ErrorCode::kInvalidState);
  EXPECT_EQ(Cash(), before);
}

TEST_F(TradingTest,
       SharedCashReservationsPreventOvercommitAndFailuresAreAtomic) {
  constexpr int64_t kUnitCash = 1001 * 10000;
  Bootstrap(3 * kUnitCash);
  (void)Submit(2);
  EXPECT_EQ(Cash(),
            (model::CashSnapshot{3 * kUnitCash, 2 * kUnitCash, kUnitCash}));
  (void)Submit(1);
  EXPECT_EQ(Cash(), (model::CashSnapshot{3 * kUnitCash, 3 * kUnitCash, 0}));
  const auto before = Cash();
  const auto position = Position();
  model::ClientOrderId out{77};
  trading.SetAcceptingRequests(true);
  EXPECT_EQ(trading.SubmitOrder(Request(1), out),
            ErrorCode::kResourceExhausted);
  trading.SetAcceptingRequests(false);
  EXPECT_NE(std::string_view(trading.error_detail()).find("cash"),
            std::string_view::npos);
  EXPECT_EQ(out.value, 77U);
  EXPECT_EQ(trading.order_count(), 2U);
  EXPECT_EQ(Cash(), before);
  EXPECT_EQ(Position(), position);
}

TEST_F(TradingTest, SharedSellReservationsExcludeUnavailableBootstrapShares) {
  Bootstrap(0, 300, 200, true);
  auto request = Request(150);
  request.side = model::OrderSide::kSell;
  (void)SubmitRequest(request);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 300, 50, 150}));
  request.quantity = 50;
  (void)SubmitRequest(request);
  const auto before = Position();
  EXPECT_EQ(before, (model::PositionSnapshot{instrument_id, 300, 0, 200}));
  request.quantity = 1;
  model::ClientOrderId out{88};
  trading.SetAcceptingRequests(true);
  EXPECT_EQ(trading.SubmitOrder(request, out), ErrorCode::kResourceExhausted);
  trading.SetAcceptingRequests(false);
  EXPECT_EQ(out.value, 88U);
  EXPECT_EQ(Position(), before);
  EXPECT_EQ(Cash(), (model::CashSnapshot{0, 0, 0}));
}

TEST_F(TradingTest, TwoInstrumentsShareCashButKeepHoldingsIndependent) {
  trading.Reset();
  market_data.Reset();
  market_config.instrument_count = 2;
  auto& other = market_config.instruments[1];
  other = market_config.instruments[0];
  ASSERT_EQ(model::MakeInstrumentId("SZSE", "000001", other.instrument.id),
            ErrorCode::kOk);
  other.instrument.venue = model::Venue::kSzse;
  std::memcpy(other.instrument.symbol, "000001", model::kSymbolLength);
  ASSERT_TRUE(market_data.Configure(market_config).ok());
  constexpr int64_t kUnitCash = 10010000;
  Bootstrap(3 * kUnitCash);
  const auto first = Submit(2);
  auto request = Request(1);
  request.instrument_id = other.instrument.id;
  const auto second = SubmitRequest(request);
  EXPECT_EQ(Cash(), (model::CashSnapshot{3 * kUnitCash, 3 * kUnitCash, 0}));
  trading.SetAcceptingRequests(true);
  model::ClientOrderId unchanged{99};
  EXPECT_EQ(trading.SubmitOrder(request, unchanged),
            ErrorCode::kResourceExhausted);
  trading.SetAcceptingRequests(false);
  EXPECT_EQ(unchanged.value, 99U);
  Accept(second);
  auto fill = Execution(second, 2, "other-instrument", 1, 1);
  fill.fill.instrument_id = other.instrument.id;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash(), (model::CashSnapshot{2 * kUnitCash, 2 * kUnitCash, 0}));
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 0, 0, 0}));
  model::PositionSnapshot second_position;
  ASSERT_TRUE(trading.TryGetPosition(other.instrument.id, second_position));
  EXPECT_EQ(second_position,
            (model::PositionSnapshot{other.instrument.id, 1, 0, 0}));
  ASSERT_EQ(
      trading.ApplyEvent(Event(first, 1, OrderUpdateKind::kRejected), dispatch),
      ErrorCode::kOk);
  EXPECT_EQ(Cash(), (model::CashSnapshot{2 * kUnitCash, 0, 2 * kUnitCash}));
  ASSERT_TRUE(trading.TryGetPosition(other.instrument.id, second_position));
  EXPECT_EQ(second_position.total_qty, 1);
  EXPECT_EQ(Position().total_qty, 0);
}

TEST_F(TradingTest,
       BuyImprovementIsImmediatelyAvailableAndNewSharesCannotBeSold) {
  constexpr int64_t kInitial = 3000000000;
  constexpr int64_t kLimitPart = 1001 * 100 * 10000;
  constexpr int64_t kActual = 1000 * 100 * 10000;
  Bootstrap(kInitial);
  const auto id = Submit(200);
  Accept(id);
  auto fill = Execution(id, 2, "improved", 100, 100);
  fill.fill.price = 1000;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash(), (model::CashSnapshot{kInitial - kActual, kLimitPart,
                                         kInitial - kActual - kLimitPart}));
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 100, 0, 0}));
  const auto once_cash = Cash();
  const auto once_position = Position();
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash(), once_cash);
  EXPECT_EQ(Position(), once_position);
  auto sell = Request(1);
  sell.side = model::OrderSide::kSell;
  model::ClientOrderId unchanged{99};
  trading.SetAcceptingRequests(true);
  EXPECT_EQ(trading.SubmitOrder(sell, unchanged),
            ErrorCode::kResourceExhausted);
  trading.SetAcceptingRequests(false);
  Cancel(id);
  EXPECT_EQ(Cash(), once_cash);
  Send();
  auto canceled = Event(id, 3, OrderUpdateKind::kCanceled, 100);
  canceled.cancel_request_id = Snapshot(id).cancel_request_id;
  ASSERT_EQ(trading.ApplyEvent(canceled, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash(),
            (model::CashSnapshot{kInitial - kActual, 0, kInitial - kActual}));
  EXPECT_EQ(Position(), once_position);
}

TEST_F(TradingTest,
       SellFillCreditsCashAndCancelRejectionKeepsRemainingReservation) {
  Bootstrap(7000000, 300, 200, true);
  auto request = Request(200);
  request.side = model::OrderSide::kSell;
  const auto id = SubmitRequest(request);
  Accept(id);
  auto fill = Execution(id, 2, "sell-part", 100, 100);
  fill.fill.price = 1002;
  trading::TradingDispatch dispatch;
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 200, 0, 100}));
  EXPECT_EQ(Cash(), (model::CashSnapshot{1009000000, 0, 1009000000}));
  const auto once_cash = Cash();
  const auto once_position = Position();
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_FALSE(dispatch.notify_order);
  EXPECT_EQ(Cash(), once_cash);
  EXPECT_EQ(Position(), once_position);
  Cancel(id);
  Send();
  auto rejected = Event(id, 3, OrderUpdateKind::kCancelRejected, 100);
  rejected.cancel_request_id = Snapshot(id).cancel_request_id;
  auto wrong_attempt = rejected;
  ++wrong_attempt.cancel_request_id.value;
  const auto pending = Snapshot(id);
  EXPECT_EQ(trading.ApplyEvent(wrong_attempt, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Snapshot(id), pending);
  EXPECT_EQ(Cash(), once_cash);
  EXPECT_EQ(Position(), once_position);
  ASSERT_EQ(trading.ApplyEvent(rejected, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 200, 0, 100}));
  Cancel(id);
  Send();
  auto canceled = Event(id, 4, OrderUpdateKind::kCanceled, 100);
  canceled.cancel_request_id = Snapshot(id).cancel_request_id;
  ASSERT_EQ(trading.ApplyEvent(canceled, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 200, 100, 0}));
  EXPECT_EQ(Cash(), (model::CashSnapshot{1009000000, 0, 1009000000}));
}

TEST_F(TradingTest,
       DefinitiveRejectionAndLocalCancellationReleaseReservations) {
  Bootstrap(100000000, 100, 100, true);
  const auto buy = Submit(1);
  auto sell_request = Request(100);
  sell_request.side = model::OrderSide::kSell;
  const auto sell = SubmitRequest(sell_request);
  Send();
  trading::TradingDispatch dispatch;
  ASSERT_EQ(
      trading.ApplyEvent(Event(buy, 1, OrderUpdateKind::kRejected), dispatch),
      ErrorCode::kOk);
  ASSERT_EQ(
      trading.ApplyEvent(Event(sell, 1, OrderUpdateKind::kRejected), dispatch),
      ErrorCode::kOk);
  EXPECT_EQ(Cash(), (model::CashSnapshot{100000000, 0, 100000000}));
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 100, 100, 0}));
  (void)Submit(1);
  (void)SubmitRequest(sell_request);
  trading.BeginStopping();
  ASSERT_TRUE(trading.ProcessRequests(clock.now).ok());
  EXPECT_EQ(Cash().reserved, 10010000);
  EXPECT_EQ(Position().reserved_qty, 100);
  while (trading.has_local_updates()) {
    ASSERT_EQ(trading.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  }
  EXPECT_EQ(Cash(), (model::CashSnapshot{100000000, 0, 100000000}));
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 100, 100, 0}));
}

TEST_F(TradingTest, ZeroAccountCannotBuyOrSellAndSendFailureKeepsReservations) {
  Bootstrap(0);
  model::ClientOrderId out{5};
  trading.SetAcceptingRequests(true);
  EXPECT_EQ(trading.SubmitOrder(Request(1), out),
            ErrorCode::kResourceExhausted);
  auto sell = Request(1);
  sell.side = model::OrderSide::kSell;
  EXPECT_EQ(trading.SubmitOrder(sell, out), ErrorCode::kResourceExhausted);
  trading.SetAcceptingRequests(false);
  EXPECT_EQ(out.value, 5U);
  EXPECT_EQ(trading.order_count(), 0U);
  Bootstrap(100000000);
  const auto id = Submit(1);
  const auto before = Cash();
  adapter.fail_send = true;
  EXPECT_FALSE(trading.ProcessRequests(clock.now).ok());
  trading.BeginStopping();
  EXPECT_TRUE(trading.ScheduleStopping(clock.now).ok());
  EXPECT_TRUE(trading.ProcessRequests(clock.now).ok());
  EXPECT_EQ(Cash(), before);
  EXPECT_EQ(Snapshot(id).state, OrderState::kPendingNew);
}

TEST_F(TradingTest, PricePrecisionZeroAndSixConvertExactlyWithoutRounding) {
  for (const uint8_t decimals : {uint8_t{0}, uint8_t{6}}) {
    SCOPED_TRACE(decimals);
    Precision(decimals);
    Bootstrap(100000000);
    const int64_t scale = decimals == 0 ? 1000000 : 1;
    auto request = Request(3);
    request.limit_price = 7;
    const auto id = SubmitRequest(request);
    EXPECT_EQ(Cash().reserved, 21 * scale);
    Accept(id);
    auto fill = Execution(id, 2, "precise", 3, 3);
    fill.fill.price = 6;
    trading::TradingDispatch dispatch;
    ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
    EXPECT_EQ(Cash(), (model::CashSnapshot{100000000 - 18 * scale, 0,
                                           100000000 - 18 * scale}));
    EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 3, 0, 0}));
  }
}

TEST_F(TradingTest, SubmitNotionalOverflowPreservesEveryOutput) {
  for (const uint8_t decimals : {uint8_t{0}, uint8_t{6}}) {
    Precision(decimals);
    Bootstrap(std::numeric_limits<int64_t>::max());
    auto request = Request(decimals == 0 ? 1 : 2);
    request.limit_price = std::numeric_limits<int64_t>::max();
    const auto before = Cash();
    model::ClientOrderId out{8};
    trading.SetAcceptingRequests(true);
    EXPECT_EQ(trading.SubmitOrder(request, out), ErrorCode::kInvalidArgument);
    trading.SetAcceptingRequests(false);
    EXPECT_EQ(out.value, 8U);
    EXPECT_EQ(trading.order_count(), 0U);
    EXPECT_EQ(Cash(), before);
    EXPECT_EQ(Position().total_qty, 0);
  }
}

TEST_F(TradingTest, SellCashOverflowDoesNotCommitOrderHashOrAccount) {
  constexpr auto kMaximum = std::numeric_limits<int64_t>::max();
  Bootstrap(kMaximum - 20000000, 1, 1, true);
  auto request = Request(1);
  request.side = model::OrderSide::kSell;
  const auto id = SubmitRequest(request);
  Accept(id);
  auto fill = Execution(id, 2, "same-id", 1, 1);
  fill.fill.price = 2001;
  const auto cash = Cash();
  const auto position = Position();
  const auto order = Snapshot(id);
  trading::TradingDispatch dispatch;
  dispatch.notify_order = true;
  EXPECT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Cash(), cash);
  EXPECT_EQ(Position(), position);
  EXPECT_EQ(Snapshot(id), order);
  EXPECT_TRUE(dispatch.notify_order);
  fill.fill.price = 1001;
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash().balance, kMaximum - 9990000);
  EXPECT_EQ(Position(), (model::PositionSnapshot{instrument_id, 0, 0, 0}));
}

TEST_F(TradingTest, FillAmountAndBoughtHoldingOverflowAreAtomic) {
  Precision(6);
  Bootstrap(0, 2, 2, true);
  auto request = Request(2);
  request.side = model::OrderSide::kSell;
  request.limit_price = 1;
  const auto sell = SubmitRequest(request);
  Accept(sell);
  auto fill = Execution(sell, 2, "overflow", 2, 2);
  fill.fill.price = std::numeric_limits<int64_t>::max();
  const auto cash = Cash();
  const auto position = Position();
  trading::TradingDispatch dispatch;
  EXPECT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Cash(), cash);
  EXPECT_EQ(Position(), position);
  fill.fill.price = 1;
  ASSERT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  EXPECT_EQ(Cash().balance, 2);
  Bootstrap(10, std::numeric_limits<int64_t>::max(), 0, true);
  request = Request(1);
  request.limit_price = 1;
  const auto buy = SubmitRequest(request);
  Accept(buy);
  fill = Execution(buy, 2, "holding-overflow", 1, 1);
  fill.fill.price = 1;
  const auto buy_cash = Cash();
  const auto buy_position = Position();
  const auto buy_order = Snapshot(buy);
  EXPECT_EQ(trading.ApplyEvent(fill, dispatch), ErrorCode::kCorrupt);
  EXPECT_EQ(Cash(), buy_cash);
  EXPECT_EQ(Position(), buy_position);
  EXPECT_EQ(Snapshot(buy), buy_order);
}

}  // namespace
}  // namespace yquant::trading
