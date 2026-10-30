#include "yquant/trading/trading_service.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/order.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_listener.h"

namespace yquant::trading {
namespace {

// Writes the source and both counter correlation fields for a rejected event.
void DiagnoseCancel(const char* reason, std::uint64_t account_id,
                    const AdapterCancelReport& report) {
  std::fprintf(stderr, "trading %s account=%" PRIu64 " caller=%" PRIu64
                       " sequence=%" PRIu64 " cancel=%" PRIu64 "\n",
               reason, account_id, report.order_id.caller_service_id,
               report.order_id.sequence, report.service_cancel_id);
}

}  // namespace

RequestResult TradingService::CancelOrder(const CancelOrderRequest& request,
                                          TradingListener& listener,
                                          internal::CallContext call) {
  ++request_depth_;
  ProcessCancelOrder(request, listener, call);
  --request_depth_;
  return RequestResult::kDelivered;
}

void TradingService::ProcessCancelOrder(const CancelOrderRequest& request,
                                        TradingListener& listener,
                                        internal::CallContext call) {
  const auto fail = [&](FailureReason reason, OrderRecord* order = nullptr) {
    Fail(request.order_id, listener, call, reason, order,
         request.cancel_request_id);
  };
  const auto found = orders_.find(request.order_id);
  if (found == orders_.end()) {
    fail(FailureReason::kUnknownOrder);
    return;
  }
  auto& order = found->second;
  if (order.order.account_id != request.account_id ||
      order.order.strategy_id != request.strategy_id) {
    fail(FailureReason::kOwnershipMismatch);
    return;
  }
  if (order.state != OrderState::kSent &&
      order.state != OrderState::kAccepted) {
    fail(FailureReason::kOrderInactive);
    return;
  }
  if (request.cancel_request_id == 0) {
    fail(FailureReason::kInvalidRequest);
    return;
  }
  if (order.cancel_ids.contains(request.cancel_request_id)) {
    fail(FailureReason::kDuplicateCancel);
    return;
  }
  if (last_cancel_id_ == std::numeric_limits<std::uint64_t>::max()) {
    fail(FailureReason::kIdExhausted);
    return;
  }
  const auto id = ++last_cancel_id_;
  order.cancel_ids.insert(request.cancel_request_id);
  auto& cancel = cancels_.emplace(
      id, CancelRecord{request, &order, &listener, call}).first->second;
  auto& adapter = account_index_.at(request.account_id)->adapter;
  cancel.send_failed = adapter.CancelOrder({request.order_id, id}) ==
                       SendResult::kNotSent;
  if (cancel.send_failed) {
    fail(FailureReason::kSendFailed, &order);
  }
}

void TradingService::AccountContext::OnCancelReport(
    const AdapterCancelReport& report) {
  owner.ApplyCancel(id, report);
}

void TradingService::ApplyCancel(std::uint64_t account_id,
                                 const AdapterCancelReport& report) {
  const auto found = cancels_.find(report.service_cancel_id);
  if (found == cancels_.end() ||
      found->second.request.account_id != account_id ||
      found->second.request.order_id != report.order_id ||
      (report.result != CancelResult::kCanceled &&
       report.result != CancelResult::kRejected)) {
    DiagnoseCancel("invalid_cancel_report", account_id, report);
    return;
  }
  auto& cancel = found->second;
  if (cancel.result) {
    if (*cancel.result != report.result) {
      DiagnoseCancel("contradictory_cancel_report", account_id, report);
    }
    return;
  }
  auto& order = *cancel.order;
  if (report.result == CancelResult::kCanceled &&
      order.state != OrderState::kSent &&
      order.state != OrderState::kAccepted &&
      order.state != OrderState::kCanceled) {
    DiagnoseCancel("inactive_order_cancel_report", account_id, report);
    return;
  }
  if (cancel.send_failed) {
    DiagnoseCancel("report_after_cancel_send_failure", account_id, report);
  }
  cancel.result = report.result;
  const bool changed = report.result == CancelResult::kCanceled &&
                       order.state != OrderState::kCanceled;
  if (changed) {
    order.state = OrderState::kCanceled;
    ++order.version;
  }
  const auto snapshot = MakeSnapshot(order);
  if (changed) {
    Notify({order.listener, OrderReport{snapshot}, &order, order.call});
  }
  Notify({cancel.listener,
          CancelReport{report.order_id, cancel.request.cancel_request_id,
                       report.result, snapshot}, &order, cancel.call});
}

}  // namespace yquant::trading
