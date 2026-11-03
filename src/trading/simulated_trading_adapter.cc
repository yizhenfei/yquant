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

void SimulatedTradingAdapter::SetInitialState(
    const FundsInfo& funds, std::span<const PositionInfo> positions) {
  initial_funds_ = funds;
  initial_positions_.assign(positions.begin(), positions.end());
}

bool SimulatedTradingAdapter::StartAccountSync(std::uint64_t sync_id) {
  if (callback_ == nullptr || sync_id == 0) {
    return false;
  }
  ++sync_requests_;
  if (!initial_funds_) {
    return true;
  }
  bool unresolved = initial_funds_->frozen_margin != 0;
  for (const auto& [id, order] : orders_) {
    unresolved |= internal::IsActiveOrder(order.state);
  }
  QueueFunds({sync_id, FundsRefreshStarted{}});
  QueueFunds({sync_id, *initial_funds_});
  QueuePositions({sync_id, PositionsRefreshStarted{}});
  for (const auto& position : initial_positions_) {
    unresolved |= position.today_frozen != 0 || position.yesterday_frozen != 0;
    QueuePositions({sync_id, position});
  }
  QueuePositions(
      {sync_id, PositionsRefreshCompleted{initial_positions_.size()}});
  QueueAccountSync({sync_id, unresolved ? AccountSyncResult::kUnresolvedOrders
                                       : AccountSyncResult::kComplete});
  return true;
}

void SimulatedTradingAdapter::QueueAccountSync(
    const AdapterAccountSyncReport& report) {
  pending_.emplace_back(report);
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
  if (running_ || callback_ == nullptr) {
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
    } else if (const auto* trade = std::get_if<TradeInfo>(&event)) {
      EmitTrade(*trade);
    } else if (const auto* report = std::get_if<AdapterFundsReport>(&event)) {
      callback_->OnFundsReport(*report);
    } else if (const auto* report =
                   std::get_if<AdapterPositionsReport>(&event)) {
      callback_->OnPositionsReport(*report);
    } else {
      callback_->OnAccountSyncReport(std::get<AdapterAccountSyncReport>(event));
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
  if (report.result == CancelResult::kCanceled) {
    report.filled_quantity = found->second.filled_quantity;
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

void SimulatedTradingAdapter::QueueFunds(const AdapterFundsReport& report) {
  pending_.emplace_back(report);
}

void SimulatedTradingAdapter::QueuePositions(
    const AdapterPositionsReport& report) {
  pending_.emplace_back(report);
}

const OrderInfo* SimulatedTradingAdapter::FindOrder(
    const OrderId& id) const {
  const auto found = orders_.find(id);
  return found == orders_.end() ? nullptr : &found->second.order;
}

}  // namespace yquant::trading
