#include "yquant/trading/simulated_trading_adapter.h"

#include <cstddef>
#include <utility>

#include "yquant/trading/order.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

SimulatedTradingAdapter::SimulatedTradingAdapter() {
  orders_.reserve(256);
  pending_.reserve(256);
  pending_cancels_.reserve(256);
}

void SimulatedTradingAdapter::SetCallback(ITradingAdapterCallback& callback) {
  callback_ = &callback;
}

SendResult SimulatedTradingAdapter::SubmitOrder(
    const SubmitOrderRequest& request) {
  const auto& order = request.order;
  const auto behavior = std::exchange(next_, SimulatedSubmit{});
  if (callback_ == nullptr || orders_.contains(order.order_id)) {
    return SendResult::kNotSent;
  }
  if (behavior.send_result != SendResult::kNotSent || behavior.report) {
    orders_.emplace(order.order_id, SimulatedOrder{order});
  }
  if (behavior.report) {
    pending_.push_back({order.order_id, *behavior.report});
  }
  return behavior.send_result;
}

SendResult SimulatedTradingAdapter::CancelOrder(
    const AdapterCancelOrderRequest& request) {
  const auto behavior = std::exchange(next_cancel_, SimulatedCancel{});
  if (callback_ == nullptr) {
    return SendResult::kNotSent;
  }
  if (behavior.report) {
    const auto found = orders_.find(request.order_id);
    const bool active = found != orders_.end() &&
                        (found->second.state == OrderState::kSent ||
                         found->second.state == OrderState::kAccepted);
    pending_cancels_.push_back(
        {request.order_id, request.service_cancel_id,
         active ? *behavior.report : CancelResult::kRejected});
  }
  return behavior.send_result;
}

void SimulatedTradingAdapter::Run() {
  if (running_) {
    return;
  }
  running_ = true;
  constexpr std::size_t kBatchSize = 64;
  for (std::size_t count = 0; count < kBatchSize; ++count) {
    const bool order = head_ < pending_.size();
    const bool cancel = cancel_head_ < pending_cancels_.size();
    if (!order && !cancel) {
      break;
    }
    if (order && (!cancel || count % 2 == 0)) {
      const auto report = pending_[head_++];
      Emit(report);
    } else {
      auto report = pending_cancels_[cancel_head_++];
      const auto found = orders_.find(report.order_id);
      if (report.result == CancelResult::kCanceled &&
          (found == orders_.end() ||
           (found->second.state != OrderState::kSent &&
            found->second.state != OrderState::kAccepted))) {
        report.result = CancelResult::kRejected;
      }
      EmitCancel(report);
    }
  }
  if (cancel_head_ == pending_cancels_.size()) {
    pending_cancels_.clear();
    cancel_head_ = 0;
  }
  if (head_ == pending_.size()) {
    pending_.clear();
    head_ = 0;
  }
  running_ = false;
}

void SimulatedTradingAdapter::Emit(const AdapterOrderReport& report) {
  const auto found = orders_.find(report.order_id);
  if (found != orders_.end() && found->second.state == OrderState::kSent &&
      (report.state == AdapterOrderState::kAccepted ||
       report.state == AdapterOrderState::kRejected)) {
    found->second.state = report.state == AdapterOrderState::kAccepted
                              ? OrderState::kAccepted : OrderState::kRejected;
  }
  if (callback_ != nullptr) {
    callback_->OnOrderReport(report);
  }
}

void SimulatedTradingAdapter::EmitCancel(const AdapterCancelReport& report) {
  const auto found = orders_.find(report.order_id);
  if (found != orders_.end() && report.result == CancelResult::kCanceled &&
      (found->second.state == OrderState::kSent ||
       found->second.state == OrderState::kAccepted)) {
    found->second.state = OrderState::kCanceled;
  }
  if (callback_ != nullptr) {
    callback_->OnCancelReport(report);
  }
}

const OrderInfo* SimulatedTradingAdapter::FindOrder(
    const OrderId& id) const {
  const auto found = orders_.find(id);
  return found == orders_.end() ? nullptr : &found->second.order;
}

}  // namespace yquant::trading
