#include "simnow_cancellations.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "simnow_fields.h"
#include "simnow_order_tag.h"

namespace yquant::trading::internal {
namespace {

bool Matches(const ActionError& e, const ActionError& target) {
  return (!e.front || e.front == target.front) &&
         (!e.session || e.session == target.session) &&
         (e.ref.empty() || e.ref == target.ref) &&
         (e.exchange.empty() || e.exchange == target.exchange) &&
         (e.system_id.empty() || target.system_id.empty() || e.system_id == target.system_id) &&
         (e.symbol.empty() || e.symbol == target.symbol);
}

}  // namespace

void SimNowCancellations::Start(int front, int session) {
  attempts_.clear();
  pending_.clear();
  memo_ = CancelSessionMemo(front, session);
}

void SimNowCancellations::Register(AdapterCancelOrderRequest attempt, int id,
                                    CThostFtdcInputOrderActionField& r) {
  r.RequestID = r.OrderActionRef = id;
  r.ActionFlag = THOST_FTDC_AF_Delete;
  Copy(r.OrderMemo, memo_);
  ActionError target{id, id, r.FrontID, r.SessionID, NativeId(r.OrderRef),
                     std::string(Field(r.ExchangeID)), NativeId(r.OrderSysID),
                     std::string(Field(r.InstrumentID)), {}};
  attempts_.emplace(id, Attempt{attempt, std::move(target)});
  pending_[attempt.order_id].push_back(id);
}

void SimNowCancellations::NotSent(int id) {
  RemovePending(attempts_.at(id).request.order_id, id);
}

void SimNowCancellations::RemovePending(OrderId order, int id) {
  auto pending = pending_.find(order);
  if (pending == pending_.end()) return;
  std::erase(pending->second, id);
  if (pending->second.empty()) pending_.erase(pending);
}

void SimNowCancellations::Reject(int id, const ActionError& error, ITradingAdapterCallback& sink) {
  const int key = id ? id : (error.action_ref ? error.action_ref : error.request_id);
  const auto found = attempts_.find(key);
  if (found == attempts_.end()) return;  // Includes already completed attempts.
  if (((!id || !error.memo.empty()) && error.memo != memo_) ||
      (error.request_id && error.request_id != key) ||
      (error.action_ref && error.action_ref != key) || !Matches(error, found->second.target)) {
    std::fprintf(stderr, "simnow unassociated_cancel_error request=%d\n", key);
    return;
  }
  const auto attempt = found->second.request;
  RemovePending(attempt.order_id, key);
  attempts_.erase(found);
  sink.OnCancelReport({attempt.order_id, attempt.service_cancel_id, CancelResult::kRejected});
}

bool SimNowCancellations::Complete(OrderId order, int filled, ITradingAdapterCallback& sink) {
  auto group = pending_.extract(order);
  if (group.empty()) return false;
  for (int id : group.mapped()) {
    auto item = attempts_.extract(id);
    const auto& attempt = item.mapped().request;
    sink.OnCancelReport({order, attempt.service_cancel_id, CancelResult::kCanceled, filled});
  }
  return true;
}

}  // namespace yquant::trading::internal
