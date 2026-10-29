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
    orders_.emplace(order.order_id, order);
  }
  if (behavior.report) {
    pending_.push_back({order.order_id, *behavior.report});
  }
  return behavior.send_result;
}

void SimulatedTradingAdapter::Run() {
  if (running_) {
    return;
  }
  running_ = true;
  constexpr std::size_t kBatchSize = 64;
  for (std::size_t count = 0; count < kBatchSize && head_ < pending_.size();
       ++count) {
    const auto report = pending_[head_++];
    Emit(report);
  }
  if (head_ == pending_.size()) {
    pending_.clear();
    head_ = 0;
  }
  running_ = false;
}

void SimulatedTradingAdapter::Emit(const AdapterOrderReport& report) {
  if (callback_ != nullptr) {
    callback_->OnOrderReport(report);
  }
}

const OrderInfo* SimulatedTradingAdapter::FindOrder(
    const OrderId& id) const {
  const auto found = orders_.find(id);
  return found == orders_.end() ? nullptr : &found->second;
}

}  // namespace yquant::trading
