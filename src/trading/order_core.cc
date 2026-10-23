#include "yquant/trading/order_core.h"

#include <stddef.h>
#include <stdint.h>

#include <string_view>

#include "yquant/base/status.h"
#include "yquant/model/futures.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;

// Returns an error result without executable actions.
OrderApplyResult Failure(ErrorCode code, std::string_view detail) noexcept {
  OrderApplyResult result;
  result.code = code;
  result.diagnostic.Set(0, detail);
  return result;
}

// Checks whether the side can describe an actual order.
bool ValidSide(model::OrderSide side) noexcept {
  switch (side) {
    case model::OrderSide::kBuy:
    case model::OrderSide::kSell:
      return true;
    case model::OrderSide::kUnknown:
      return false;
  }
  return false;
}

// Rejects unknown numeric effects; account-specific compatibility is external.
bool ValidEffect(model::PositionEffect effect) noexcept {
  switch (effect) {
    case model::PositionEffect::kNone:
    case model::PositionEffect::kOpen:
    case model::PositionEffect::kCloseToday:
    case model::PositionEffect::kCloseYesterday:
    case model::PositionEffect::kClose:
      return true;
  }
  return false;
}

// Checks the original request and positive integer-unit rules.
bool ValidRequest(const model::OrderRequest& request,
                  const OrderCoreRules& rules) noexcept {
  return request.instrument_id.value != 0 && ValidSide(request.side) &&
         ValidEffect(request.position_effect) && rules.price_tick > 0 &&
         rules.quantity_step > 0 && request.limit_price > 0 &&
         request.quantity > 0 && request.limit_price % rules.price_tick == 0 &&
         request.quantity % rules.quantity_step == 0;
}

// Checks bounded reason storage before reading its terminator.
bool ValidReason(const model::OrderReason& reason) noexcept {
  return reason.size < sizeof(reason.text) && reason.text[reason.size] == '\0';
}

// Rejects local-only updates and unknown numeric kinds at the broker boundary.
bool BrokerKind(OrderUpdateKind kind) noexcept {
  switch (kind) {
    case OrderUpdateKind::kAccepted:
    case OrderUpdateKind::kRejected:
    case OrderUpdateKind::kFill:
    case OrderUpdateKind::kCanceled:
    case OrderUpdateKind::kCancelRejected:
      return true;
    case OrderUpdateKind::kCanceledBeforeSend:
    case OrderUpdateKind::kCancelNotSent:
      return false;
  }
  return false;
}

// Prevents a non-fill event from carrying an unprocessed execution.
bool EmptyFill(const model::Fill& fill) noexcept {
  return fill.client_order_id.value == 0 && fill.strategy_order_id.value == 0 &&
         fill.adapter_order_id.empty() && fill.execution_id.empty() &&
         fill.instrument_id.value == 0 &&
         fill.side == model::OrderSide::kUnknown && fill.price == 0 &&
         fill.quantity == 0 && fill.event_time_ns == 0 &&
         fill.receive_time_ns == 0 && !fill.has_event_time &&
         fill.position_effect == model::PositionEffect::kNone;
}

// Checks execution identity, units and limit price without consulting history.
bool ValidExecution(const model::TradingEvent& event,
                    const model::OrderSnapshot& order,
                    const OrderCoreRules& rules) noexcept {
  const auto& fill = event.fill;
  return fill.client_order_id == order.client_order_id &&
         fill.strategy_order_id.value == 0 &&
         fill.adapter_order_id == event.adapter_order_id &&
         !fill.execution_id.empty() &&
         fill.instrument_id == order.instrument_id && fill.side == order.side &&
         fill.position_effect == order.position_effect && fill.price > 0 &&
         fill.quantity > 0 && fill.price % rules.price_tick == 0 &&
         fill.quantity % rules.quantity_step == 0 &&
         fill.receive_time_ns == event.receive_time_ns &&
         (fill.has_event_time || fill.event_time_ns == 0) &&
         (order.side != model::OrderSide::kBuy ||
          fill.price <= order.limit_price) &&
         (order.side != model::OrderSide::kSell ||
          fill.price >= order.limit_price);
}

// Checks structural fields before duplicate suppression. Whether an unsolicited
// cancel conflicts with a current attempt is checked only for a new sequence.
bool ValidReportFields(const model::TradingEvent& event, uint64_t now,
                       const model::OrderSnapshot& order,
                       const OrderCoreRules& rules) noexcept {
  const bool fill = event.kind == OrderUpdateKind::kFill;
  const bool cancel = event.kind == OrderUpdateKind::kCanceled ||
                      event.kind == OrderUpdateKind::kCancelRejected;
  const bool unsolicited = rules.allow_unsolicited_cancel &&
                           event.kind == OrderUpdateKind::kCanceled &&
                           event.cancel_request_id.value == 0;
  if (!BrokerKind(event.kind) ||
      event.client_order_id != order.client_order_id ||
      event.report_sequence == 0 || event.receive_time_ns > now ||
      !ValidReason(event.reason) || event.cumulative_filled_qty < 0 ||
      event.cumulative_filled_qty > order.original_qty ||
      (!unsolicited && cancel != (event.cancel_request_id.value != 0)) ||
      (!fill && !EmptyFill(event.fill)) ||
      ((fill || event.kind == OrderUpdateKind::kAccepted) &&
       event.adapter_order_id.empty())) {
    return false;
  }
  return !fill || ValidExecution(event, order, rules);
}

// Creates a report result whose shared payload starts from the current order.
OrderApplyResult ReportResult(const model::OrderSnapshot& order,
                              const model::TradingEvent& report) noexcept {
  OrderApplyResult result;
  result.update.order = order;
  result.update.kind = report.kind;
  result.update.cancel_request_id = report.cancel_request_id;
  result.update.reason = report.reason;
  return result;
}

// Applies acceptance without reversing a lifecycle already confirmed by fills.
OrderApplyResult AcceptReport(const model::OrderSnapshot& order,
                              const model::TradingEvent& report) noexcept {
  auto result = ReportResult(order, report);
  switch (order.state) {
    case OrderState::kPendingNew:
      result.update.order.state = OrderState::kWorking;
      result.actions[result.action_count++] =
          OrderActionKind::kPublishOrderUpdate;
      return result;
    case OrderState::kWorking:
    case OrderState::kFilled:
    case OrderState::kCanceled:
      return result;
    case OrderState::kRejected:
      return Failure(ErrorCode::kCorrupt,
                     "acceptance would reverse a rejected order");
  }
  return Failure(ErrorCode::kCorrupt, "invalid order state");
}

// Applies a unique execution after the host has checked its history key.
OrderApplyResult FillReport(const model::OrderSnapshot& order,
                            const model::TradingEvent& report) noexcept {
  if (model::IsTerminalOrderState(order.state) ||
      report.fill.quantity > order.original_qty - order.cumulative_filled_qty ||
      report.cumulative_filled_qty !=
          order.cumulative_filled_qty + report.fill.quantity) {
    return Failure(ErrorCode::kCorrupt,
                   "execution reverses terminal state or overfills order");
  }
  auto result = ReportResult(order, report);
  auto& next = result.update.order;
  next.cumulative_filled_qty = report.cumulative_filled_qty;
  next.leaves_qty = order.original_qty - next.cumulative_filled_qty;
  next.state =
      next.leaves_qty == 0 ? OrderState::kFilled : OrderState::kWorking;
  result.fill = report.fill;
  result.actions[result.action_count++] = OrderActionKind::kApplyExecution;
  result.actions[result.action_count++] = OrderActionKind::kPublishOrderUpdate;
  return result;
}

// Ends only the matched business attempt; the executor checks send progress.
OrderApplyResult CancelReport(const model::OrderSnapshot& order,
                              const model::TradingEvent& report,
                              const OrderCoreRules& rules) noexcept {
  const bool unsolicited = rules.allow_unsolicited_cancel &&
                           report.kind == OrderUpdateKind::kCanceled &&
                           report.cancel_request_id.value == 0 &&
                           !order.cancel_pending;
  if (!unsolicited && (!order.cancel_pending ||
                       report.cancel_request_id != order.cancel_request_id ||
                       (order.state != OrderState::kWorking &&
                        order.state != OrderState::kFilled))) {
    return Failure(ErrorCode::kCorrupt,
                   "cancel result does not match the pending attempt");
  }
  auto result = ReportResult(order, report);
  if (report.kind == OrderUpdateKind::kCanceled) {
    if (order.state != OrderState::kWorking &&
        !(unsolicited && order.state == OrderState::kPendingNew)) {
      return Failure(ErrorCode::kCorrupt,
                     "cancellation would reverse a terminal order");
    }
    result.update.order.state = OrderState::kCanceled;
    result.update.order.leaves_qty = 0;
    result.actions[result.action_count++] =
        OrderActionKind::kReleaseRemainingReservation;
  }
  result.update.order.cancel_pending = false;
  result.actions[result.action_count++] = OrderActionKind::kPublishOrderUpdate;
  return result;
}

// Selects the business transition after field and sequence validation.
OrderApplyResult TransitionReport(const model::OrderSnapshot& order,
                                  const model::TradingEvent& report,
                                  const OrderCoreRules& rules) noexcept {
  switch (report.kind) {
    case OrderUpdateKind::kAccepted:
      return AcceptReport(order, report);
    case OrderUpdateKind::kRejected: {
      if (order.state != OrderState::kPendingNew ||
          order.cumulative_filled_qty != 0) {
        return Failure(ErrorCode::kCorrupt,
                       "new-order rejection after acceptance or fill");
      }
      auto result = ReportResult(order, report);
      result.update.order.state = OrderState::kRejected;
      result.update.order.leaves_qty = 0;
      result.actions[result.action_count++] =
          OrderActionKind::kReleaseRemainingReservation;
      result.actions[result.action_count++] =
          OrderActionKind::kPublishOrderUpdate;
      return result;
    }
    case OrderUpdateKind::kFill:
      return FillReport(order, report);
    case OrderUpdateKind::kCanceled:
    case OrderUpdateKind::kCancelRejected:
      return CancelReport(order, report, rules);
    case OrderUpdateKind::kCanceledBeforeSend:
    case OrderUpdateKind::kCancelNotSent:
      return Failure(ErrorCode::kCorrupt, "local result received from adapter");
  }
  return Failure(ErrorCode::kCorrupt, "unknown broker event");
}

// Checks lifecycle invariants without consulting execution or request storage.
bool ValidSnapshotState(const model::OrderSnapshot& order) noexcept {
  switch (order.state) {
    case OrderState::kPendingNew:
      return order.cumulative_filled_qty == 0 && order.report_sequence == 0 &&
             order.adapter_order_id.empty() && !order.cancel_pending;
    case OrderState::kWorking:
      return order.report_sequence != 0 && !order.adapter_order_id.empty() &&
             order.cumulative_filled_qty < order.original_qty;
    case OrderState::kFilled:
      return order.report_sequence != 0 && !order.adapter_order_id.empty() &&
             order.cumulative_filled_qty == order.original_qty;
    case OrderState::kCanceled:
      return !order.cancel_pending &&
             order.cumulative_filled_qty < order.original_qty &&
             (order.report_sequence != 0 || (order.cumulative_filled_qty == 0 &&
                                             order.adapter_order_id.empty()));
    case OrderState::kRejected:
      return order.cumulative_filled_qty == 0 && !order.cancel_pending;
  }
  return false;
}

// Validates stable attributes and derived quantities before lifecycle checks.
bool ValidSnapshot(const model::OrderSnapshot& order,
                   const OrderCoreRules& rules) noexcept {
  const model::OrderRequest request{order.instrument_id, order.side,
                                    order.limit_price, order.original_qty,
                                    order.position_effect};
  if (!ValidRequest(request, rules) || order.client_order_id.value == 0 ||
      order.strategy_order_id.value != 0 || order.cumulative_filled_qty < 0 ||
      order.cumulative_filled_qty > order.original_qty ||
      order.cumulative_filled_qty % rules.quantity_step != 0 ||
      (order.cancel_pending && order.cancel_request_id.value == 0) ||
      (order.report_sequence == 0 && order.receive_time_ns != 0) ||
      (order.report_sequence != 0 && order.adapter_order_id.empty() &&
       order.state != OrderState::kRejected)) {
    return false;
  }
  const auto leaves = model::IsTerminalOrderState(order.state)
                          ? 0
                          : order.original_qty - order.cumulative_filled_qty;
  return order.leaves_qty == leaves && ValidSnapshotState(order);
}
}  // namespace

OrderApplyResult OrderCore::Apply(const OrderEvent& event) noexcept {
  if (event.kind != OrderEventKind::kCreated &&
      (!initialized_ || event.client_order_id != snapshot_.client_order_id)) {
    return Failure(ErrorCode::kInvalidState,
                   "event has no matching initialized order");
  }
  switch (event.kind) {
    case OrderEventKind::kCreated:
      return Create(event);
    case OrderEventKind::kCancelRequested:
      return RequestCancel(event);
    case OrderEventKind::kBrokerReport:
      return ApplyReport(event);
    case OrderEventKind::kCanceledBeforeSend:
    case OrderEventKind::kCancelNotSent:
      return ApplyLocalResult(event);
  }
  return Failure(ErrorCode::kInvalidArgument, "unknown order event");
}

OrderApplyResult OrderCore::Create(const OrderEvent& event) noexcept {
  if (initialized_) {
    return Failure(ErrorCode::kInvalidState, "order is already initialized");
  }
  if (event.client_order_id.value == 0 ||
      !ValidRequest(event.request, event.rules)) {
    return Failure(ErrorCode::kInvalidArgument, "invalid order or unit rules");
  }
  model::OrderSnapshot next;
  next.client_order_id = event.client_order_id;
  next.instrument_id = event.request.instrument_id;
  next.side = event.request.side;
  next.limit_price = event.request.limit_price;
  next.original_qty = event.request.quantity;
  next.leaves_qty = event.request.quantity;
  next.position_effect = event.request.position_effect;
  OrderApplyResult result;
  result.request = event.request;
  result.update.order = next;
  result.actions[result.action_count++] = OrderActionKind::kSubmitOrder;
  snapshot_ = next;
  rules_ = event.rules;
  initialized_ = true;
  return result;
}

OrderApplyResult OrderCore::RequestCancel(const OrderEvent& event) noexcept {
  if (snapshot_.state != OrderState::kWorking || snapshot_.cancel_pending) {
    return Failure(ErrorCode::kInvalidState,
                   "order is not cancellable or has a pending attempt");
  }
  if (event.cancel_request_id.value == 0 ||
      event.cancel_request_id.value <= snapshot_.cancel_request_id.value) {
    return Failure(ErrorCode::kInvalidArgument, "cancel attempt ID is not new");
  }
  auto next = snapshot_;
  next.cancel_request_id = event.cancel_request_id;
  next.cancel_pending = true;
  OrderApplyResult result;
  result.update.order = next;
  result.update.cancel_request_id = event.cancel_request_id;
  result.actions[result.action_count++] = OrderActionKind::kCancelOrder;
  snapshot_ = next;
  return result;
}

OrderApplyResult OrderCore::ApplyLocalResult(const OrderEvent& event) noexcept {
  if (!ValidReason(event.reason)) {
    return Failure(ErrorCode::kCorrupt, "invalid local result reason");
  }
  auto next = snapshot_;
  OrderApplyResult result;
  result.update.reason = event.reason;
  if (event.kind == OrderEventKind::kCanceledBeforeSend) {
    if (next.state != OrderState::kPendingNew ||
        next.cumulative_filled_qty != 0 || next.report_sequence != 0 ||
        !next.adapter_order_id.empty() || next.cancel_pending ||
        event.cancel_request_id.value != 0) {
      return Failure(ErrorCode::kCorrupt,
                     "local new-order cancellation conflicts with order");
    }
    next.state = OrderState::kCanceled;
    next.leaves_qty = 0;
    result.update.kind = OrderUpdateKind::kCanceledBeforeSend;
    result.actions[result.action_count++] =
        OrderActionKind::kReleaseRemainingReservation;
  } else {
    if (!model::IsTerminalOrderState(next.state) || !next.cancel_pending ||
        event.cancel_request_id.value == 0 ||
        event.cancel_request_id != next.cancel_request_id) {
      return Failure(ErrorCode::kCorrupt,
                     "local cancel result does not match terminal attempt");
    }
    next.cancel_pending = false;
    result.update.kind = OrderUpdateKind::kCancelNotSent;
    result.update.cancel_request_id = event.cancel_request_id;
  }
  result.update.order = next;
  result.actions[result.action_count++] = OrderActionKind::kPublishOrderUpdate;
  snapshot_ = next;
  return result;
}

OrderApplyResult OrderCore::ApplyReport(const OrderEvent& event) noexcept {
  const auto& report = event.report;
  if (!ValidReportFields(report, event.now, snapshot_, rules_)) {
    return Failure(ErrorCode::kCorrupt, "invalid report fields or timestamp");
  }
  if (report.report_sequence <= snapshot_.report_sequence) {
    OrderApplyResult result;
    result.outcome = OrderApplyOutcome::kIgnored;
    result.update.order = snapshot_;
    return result;
  }
  if ((snapshot_.state == OrderState::kCanceled &&
       snapshot_.report_sequence == 0) ||
      report.report_sequence != snapshot_.report_sequence + 1 ||
      (!snapshot_.adapter_order_id.empty() &&
       !report.adapter_order_id.empty() &&
       snapshot_.adapter_order_id != report.adapter_order_id)) {
    return Failure(ErrorCode::kCorrupt,
                   "report sequence or broker identity mismatch");
  }
  if (report.kind != OrderUpdateKind::kFill &&
      report.cumulative_filled_qty != snapshot_.cumulative_filled_qty) {
    return Failure(ErrorCode::kCorrupt,
                   "non-fill cumulative quantity disagrees with order");
  }
  auto result = TransitionReport(snapshot_, report, rules_);
  if (!result.ok()) {
    return result;
  }
  auto& next = result.update.order;
  if (!report.adapter_order_id.empty()) {
    next.adapter_order_id = report.adapter_order_id;
  }
  next.report_sequence = report.report_sequence;
  next.receive_time_ns = report.receive_time_ns;
  snapshot_ = next;
  return result;
}

OrderRestoreResult OrderCore::Restore(const model::OrderSnapshot& snapshot,
                                      const OrderCoreRules& rules) noexcept {
  OrderRestoreResult result;
  if (!ValidSnapshot(snapshot, rules)) {
    result.code = ErrorCode::kCorrupt;
    result.diagnostic.Set(0, "invalid restored order or unit rules");
    return result;
  }
  result.core.snapshot_ = snapshot;
  result.core.rules_ = rules;
  result.core.initialized_ = true;
  return result;
}

}  // namespace yquant::trading
