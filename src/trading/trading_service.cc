#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/internal/order_state.h"
#include "yquant/trading/order.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_listener.h"

namespace yquant::trading {
namespace {

// Rejects invalid parameters before recording an order or invoking its adapter.
bool Valid(const OrderInfo& order) {
  return order.order_id.sequence != 0 && order.instrument_id != 0 &&
         std::isfinite(order.price) && order.price > 0 &&
         order.quantity > 0 &&
         (order.side == Side::kBuy || order.side == Side::kSell) &&
         (order.offset == Offset::kOpen || order.offset == Offset::kClose ||
          order.offset == Offset::kCloseToday ||
          order.offset == Offset::kCloseYesterday);
}

// Checks a counter report before applying it to an established order.
bool ValidReport(OrderState current, std::int32_t total,
                 const AdapterOrderReport& report) {
  if (report.state == AdapterOrderState::kRejected) {
    return report.filled_quantity == 0 &&
           (current == OrderState::kSent || current == OrderState::kSendFailed ||
            current == OrderState::kRejected);
  }
  return report.state == AdapterOrderState::kAccepted &&
         report.filled_quantity >= 0 && report.filled_quantity <= total &&
         current != OrderState::kRejected;
}

}  // namespace

void TradingService::Diagnose(const char* message, std::uint64_t account_id,
                              OrderId id) {
  std::fprintf(stderr, "trading %s account=%" PRIu64 " caller=%" PRIu64
                       " sequence=%" PRIu64 "\n",
               message, account_id, id.caller_service_id, id.sequence);
}

TradingService::TradingService() {
  orders_.reserve(256);
  cancels_.reserve(256);
  pending_.resize(256);
}

bool TradingService::BindAccount(std::uint64_t account_id,
                                ITradingAdapter& adapter) {
  if (running_ || request_depth_ || callback_depth_ ||
      account_index_.contains(account_id)) {
    return false;
  }
  for (const auto& account : accounts_) {
    if (&account->adapter == &adapter) {
      return false;
    }
  }
  auto account = std::make_unique<AccountContext>(*this, account_id, adapter);
  account_index_.emplace(account_id, account.get());
  accounts_.push_back(std::move(account));
  adapter.SetCallback(*accounts_.back());
  return true;
}

RequestResult TradingService::SubmitOrder(const SubmitOrderRequest& request,
                                          TradingListener& listener,
                                          internal::CallContext call) {
  ++request_depth_;
  ProcessSubmitOrder(request, listener, call);
  --request_depth_;
  return RequestResult::kDelivered;
}

void TradingService::ProcessSubmitOrder(const SubmitOrderRequest& request,
                                        TradingListener& listener,
                                        internal::CallContext call) {
  const auto& info = request.order;
  const auto account = account_index_.find(info.account_id);
  if (account == account_index_.end()) {
    Fail(info.order_id, listener, call, FailureReason::kUnknownAccount);
    return;
  }
  if (orders_.contains(info.order_id)) {
    Fail(info.order_id, listener, call, FailureReason::kDuplicateOrder);
    return;
  }
  if (!Valid(info)) {
    Fail(info.order_id, listener, call, FailureReason::kInvalidRequest);
    return;
  }

  const auto inserted = orders_.emplace(
      info.order_id, OrderRecord{info, &listener, call});
  auto& order = inserted.first->second;
  const auto result = account->second->adapter.SubmitOrder(request);

  order.state =
      result == SendResult::kSent ? OrderState::kSent : OrderState::kSendFailed;
  ++order.version;
  if (result == SendResult::kNotSent) {
    Fail(info.order_id, listener, call, FailureReason::kSendFailed, &order);
  }
}

bool TradingService::QueryOrder(const QueryOrderRequest& query,
                                OrderSnapshot& snapshot) const {
  const auto found = orders_.find(query.order_id);
  if (found == orders_.end() ||
      found->second.order.account_id != query.account_id ||
      found->second.order.strategy_id != query.strategy_id) {
    return false;
  }
  snapshot = MakeSnapshot(found->second);
  return true;
}

OrderSnapshot TradingService::MakeSnapshot(const OrderRecord& order) {
  return {order.order, order.state, order.reported_filled_quantity, order.version};
}

void TradingService::AccountContext::OnOrderReport(
    const AdapterOrderReport& report) {
  owner.Apply(id, report);
}

void TradingService::Apply(std::uint64_t account_id,
                            const AdapterOrderReport& report) {
  const auto found = orders_.find(report.order_id);
  if (found == orders_.end() || found->second.order.account_id != account_id) {
    Diagnose("invalid_report", account_id, report.order_id);
    return;
  }
  auto& order = found->second;
  if (!ValidReport(order.state, order.order.quantity, report)) {
    Diagnose("invalid_report", account_id, report.order_id);
    return;
  }
  const auto filled = std::max(order.reported_filled_quantity,
                               report.filled_quantity);
  auto next = order.state;
  if (next == OrderState::kSent || next == OrderState::kSendFailed) {
    next = report.state == AdapterOrderState::kAccepted
               ? OrderState::kAccepted : OrderState::kRejected;
  }
  next = internal::StateAfterFill(next, filled, order.order.quantity);
  if (order.state == next && order.reported_filled_quantity == filled) {
    return;
  }
  if (order.state == OrderState::kSendFailed) {
    Diagnose("report_after_send_failure", account_id, report.order_id);
  }
  order.state = next;
  order.reported_filled_quantity = filled;
  ++order.version;
  Notify({order.listener, OrderReport{MakeSnapshot(order)}, &order,
          order.call});
}

void TradingService::Fail(OrderId order_id, TradingListener& listener,
                          internal::CallContext call,
                          FailureReason reason,
                          OrderRecord* record,
                          std::optional<std::uint64_t> cancel_id) {
  RequestFailure failure{order_id, reason, std::nullopt, cancel_id};
  if (record != nullptr) {
    failure.snapshot = MakeSnapshot(*record);
  }
  Notify({&listener, failure, record, call});
}

void TradingService::Notify(Notification notification) {
  if (request_depth_ != 0 ||
      (notification.order != nullptr && notification.order->pending != 0)) {
    if (notification.order != nullptr) {
      ++notification.order->pending;
    }
    if (pending_count_ == pending_.size()) {
      const auto old_size = pending_.size();
      pending_.resize(old_size * 2);
      // Move the wrapped prefix after the old end to preserve queue order.
      std::move(pending_.begin(), pending_.begin() + head_,
                pending_.begin() + old_size);
    }
    pending_[(head_ + pending_count_) % pending_.size()] =
        std::move(notification);
    ++pending_count_;
  } else {
    Dispatch(notification);
  }
}

void TradingService::Dispatch(const Notification& notification) {
  ++callback_depth_;
  if (const auto* failure = std::get_if<RequestFailure>(&notification.event)) {
    notification.listener->OnRequestFailure(*failure);
  } else if (const auto* report =
                 std::get_if<OrderReport>(&notification.event)) {
    notification.listener->OnOrderReport(*report);
  } else {
    notification.listener->OnCancelReport(
        std::get<CancelReport>(notification.event));
  }
  --callback_depth_;
}

void TradingService::Run() {
  if (running_ || request_depth_ != 0 || callback_depth_ != 0) {
    return;
  }
  running_ = true;
  constexpr std::size_t kBatchSize = 64;
  for (std::size_t count = 0; count < kBatchSize && pending_count_ != 0;
       ++count) {
    const auto notification = std::move(pending_[head_]);
    head_ = (head_ + 1) % pending_.size();
    --pending_count_;
    if (notification.order != nullptr) {
      --notification.order->pending;
    }
    Dispatch(notification);
  }
  if (pending_count_ == 0) {
    head_ = 0;
  }
  for (const auto& account : accounts_) {
    account->adapter.Run();
  }
  running_ = false;
}

}  // namespace yquant::trading
