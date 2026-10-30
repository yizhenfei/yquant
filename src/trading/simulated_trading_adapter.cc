#include "yquant/trading/simulated_trading_adapter.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>

#include "yquant/trading/internal/order_state.h"
#include "yquant/trading/order.h"
#include "yquant/trading/trade.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

SimulatedTradingAdapter::SimulatedTradingAdapter() {
  orders_.reserve(256);
  pending_.reserve(256);
  trades_.reserve(256);
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
    const auto state = behavior.report == AdapterOrderState::kRejected
                           ? OrderState::kRejected : OrderState::kSent;
    orders_.emplace(order.order_id, SimulatedOrder{order, state});
  }
  if (behavior.report) {
    pending_.emplace_back(AdapterOrderReport{order.order_id, *behavior.report});
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
                        internal::IsActiveOrder(found->second.state);
    pending_.emplace_back(AdapterCancelReport{
        request.order_id, request.service_cancel_id,
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
  for (std::size_t count = 0; count < kBatchSize && head_ < pending_.size();
       ++count) {
    // Move before callbacks: submitting from a callback can grow pending_.
    const auto event = std::move(pending_[head_++]);
    if (const auto* report = std::get_if<AdapterOrderReport>(&event)) {
      Emit(*report);
    } else if (const auto* report = std::get_if<AdapterCancelReport>(&event)) {
      DeliverCancel(*report);
    } else {
      EmitTrade(std::get<TradeInfo>(event));
    }
  }
  if (head_ == pending_.size()) {
    pending_.clear();
    head_ = 0;
  }
  running_ = false;
}

void SimulatedTradingAdapter::Emit(const AdapterOrderReport& report) {
  const auto found = orders_.find(report.order_id);
  if (found != orders_.end()) {
    auto& order = found->second;
    if (report.state == AdapterOrderState::kRejected &&
        report.filled_quantity == 0 && order.state == OrderState::kSent) {
      order.state = OrderState::kRejected;
    } else if (report.state == AdapterOrderState::kAccepted &&
               report.filled_quantity >= 0 &&
               report.filled_quantity <= order.order.quantity &&
               order.state != OrderState::kRejected) {
      order.filled_quantity = std::max(order.filled_quantity,
                                      report.filled_quantity);
      const auto state = order.state == OrderState::kSent
                             ? OrderState::kAccepted : order.state;
      order.state = internal::StateAfterFill(
          state, order.filled_quantity, order.order.quantity);
    }
  }
  if (callback_ != nullptr) {
    callback_->OnOrderReport(report);
  }
}

void SimulatedTradingAdapter::DeliverCancel(AdapterCancelReport report) {
  const auto found = orders_.find(report.order_id);
  if (report.result == CancelResult::kCanceled &&
      (found == orders_.end() ||
       !internal::IsActiveOrder(found->second.state))) {
    report.result = CancelResult::kRejected;
  }
  EmitCancel(report);
}

bool SimulatedTradingAdapter::Fill(const TradeInfo& trade) {
  const auto found = orders_.find(trade.order_id);
  if (found == orders_.end() || !internal::ValidTrade(trade) ||
      !internal::IsActiveOrder(found->second.state) ||
      trades_.contains(trade.trade_id)) {
    return false;
  }
  auto& order = found->second;
  if (trade.quantity > order.order.quantity - order.filled_quantity) {
    return false;
  }
  trades_.insert(trade.trade_id);
  order.filled_quantity += trade.quantity;
  order.state = internal::StateAfterFill(
      order.state, order.filled_quantity, order.order.quantity);
  pending_.emplace_back(trade);
  return true;
}

void SimulatedTradingAdapter::EmitTrade(const TradeInfo& trade) {
  if (callback_ != nullptr) {
    callback_->OnTradeReport(trade);
  }
}

void SimulatedTradingAdapter::EmitCancel(const AdapterCancelReport& report) {
  const auto found = orders_.find(report.order_id);
  if (found != orders_.end() && report.result == CancelResult::kCanceled &&
      internal::IsActiveOrder(found->second.state)) {
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
