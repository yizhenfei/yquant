#ifndef YQUANT_TESTS_MOCK_TRADING_SERVICE_H_
#define YQUANT_TESTS_MOCK_TRADING_SERVICE_H_

#include <algorithm>
#include <vector>

#include "yquant/md/instrument_scope.h"
#include "yquant/trading/trading_service.h"

namespace yquant::testing {

// Scripted strategy-service double. Records requests and commits supplied
// reports/account views; it never connects to a broker or runs a matching or
// reservation engine. Tests prepare scripts outside Run. Access is serial.
class MockTradingService final : public trading::TradingService {
 public:
  struct Event {
    std::uint64_t deadline = 0;
    trading::TradingDispatch dispatch;
    bool update_account = false;
    trading::AccountView account;
  };
  std::vector<Event> events;
  std::vector<Event> stopping_events;
  trading::AccountView initial_account;
  base::Status init_result;
  base::Status send_result;
  base::Status poll_result;
  base::Status stop_result;
  base::ErrorCode submit_result = base::ErrorCode::kOk;
  bool never_drained = false;
  int init_calls = 0;
  int send_calls = 0;
  int poll_calls = 0;
  int stop_calls = 0;
  int cancel_calls = 0;
  int delivered = 0;

  base::Status Init(const trading::TradingClientConfig&,
                    const md::MarketDataConfig& dictionary) noexcept override {
    ++init_calls;
    stage_ = "trading_connect";
    if (!init_result.ok()) return init_result;
    const auto status = instruments_.Configure(dictionary);
    if (!status.ok()) return status;
    orders_.reserve(trading::kMaxTradingOrders);
    attempted_.reserve(trading::kMaxTradingOrders);
    account_ = initial_account;
    initialized_ = true;
    return {};
  }
  base::Status Stop() noexcept override {
    ++stop_calls;
    accepting_ = false;
    stage_ = "trading_stop";
    return stop_result;
  }
  // Retains counters for post-run assertions; invalidates service queries.
  void Reset() noexcept override {
    initialized_ = false;
    accepting_ = false;
    stopping_ = false;
    orders_.clear();
    attempted_.clear();
    next_event_ = 0;
    instruments_.Reset();
  }
  void SetAcceptingRequests(bool accepting) noexcept override {
    accepting_ = accepting && !stopping_;
  }
  void BeginStopping() noexcept override {
    accepting_ = false;
    if (stopping_) return;
    stopping_ = true;
    events.insert(events.end(), stopping_events.begin(), stopping_events.end());
  }
  bool accepting_orders() const noexcept override {
    return initialized_ && accepting_ && !stopping_;
  }
  base::ErrorCode SubmitOrder(const model::OrderRequest& request,
                              model::ClientOrderId& out) noexcept override {
    if (!accepting_orders()) return base::ErrorCode::kInvalidState;
    if (submit_result != base::ErrorCode::kOk) return submit_result;
    if (orders_.size() == trading::kMaxTradingOrders)
      return base::ErrorCode::kResourceExhausted;
    model::OrderSnapshot order;
    order.client_order_id = {orders_.size() + 1};
    order.instrument_id = request.instrument_id;
    order.side = request.side;
    order.limit_price = request.limit_price;
    order.original_qty = order.leaves_qty = request.quantity;
    order.state = model::OrderState::kPendingNew;
    orders_.push_back(order);
    attempted_.push_back(false);
    out = order.client_order_id;
    return base::ErrorCode::kOk;
  }
  base::ErrorCode CancelOrder(model::ClientOrderId id) noexcept override {
    if (!accepting_orders()) return base::ErrorCode::kInvalidState;
    if (id.value == 0 || id.value > orders_.size())
      return base::ErrorCode::kInvalidArgument;
    auto& order = orders_[id.value - 1];
    if (order.state != model::OrderState::kWorking || order.cancel_pending)
      return base::ErrorCode::kInvalidState;
    ++cancel_calls;
    order.cancel_pending = true;
    order.cancel_request_id = {static_cast<std::uint64_t>(cancel_calls)};
    return base::ErrorCode::kOk;
  }
  bool TryGetOrder(model::ClientOrderId id,
                   model::OrderSnapshot& out) const noexcept override {
    if (!initialized_ || id.value == 0 || id.value > orders_.size())
      return false;
    out = orders_[id.value - 1];
    return true;
  }
  bool TryGetCash(model::CashSnapshot& out) const noexcept override {
    if (!initialized_) return false;
    out = account_.cash;
    return true;
  }
  bool TryGetPosition(model::InstrumentId id,
                      model::PositionSnapshot& out) const noexcept override {
    if (!initialized_ || !instruments_.HasInstrument(id)) return false;
    for (std::size_t i = 0; i < account_.position_count; ++i)
      if (account_.positions[i].instrument_id == id) {
        out = account_.positions[i];
        return true;
      }
    out = {id, 0, 0, 0};
    return true;
  }
  base::Status ProcessRequests(std::uint64_t now,
                               std::size_t limit = 64) noexcept override {
    stage_ = "trading_send";
    for (std::size_t i = 0; i < orders_.size() && limit != 0; ++i) {
      if (attempted_[i]) continue;
      --limit;
      attempted_[i] = true;
      if (stopping_) {
        Event event;
        event.deadline = now;
        event.dispatch.notify_order = true;
        event.dispatch.update.kind =
            model::OrderUpdateKind::kCanceledBeforeSend;
        event.dispatch.update.order = orders_[i];
        event.dispatch.update.order.state = model::OrderState::kCanceled;
        event.dispatch.update.order.leaves_qty = 0;
        events.push_back(event);
      } else {
        ++send_calls;
        if (!send_result.ok()) return send_result;
      }
    }
    return {};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    for (const bool attempted : attempted_)
      if (!attempted) return 0;
    if (next_event_ < events.size()) return events[next_event_].deadline;
    return {};
  }
  bool has_ready_event(std::uint64_t now) const noexcept override {
    if (next_event_ == events.size()) return false;
    const auto id =
        events[next_event_].dispatch.update.order.client_order_id.value;
    return id != 0 && id <= attempted_.size() && attempted_[id - 1] &&
           events[next_event_].deadline <= now;
  }
  bool is_drained() const noexcept override {
    if (!initialized_) return true;
    if (!stopping_ || never_drained || next_event_ < events.size())
      return false;
    return std::all_of(orders_.begin(), orders_.end(), [](const auto& order) {
      return model::IsTerminalOrderState(order.state) && !order.cancel_pending;
    });
  }
  std::size_t order_count() const noexcept override { return orders_.size(); }
  const char* error_detail() const noexcept override {
    return "injected trading service failure";
  }
  const char* error_stage() const noexcept override { return stage_; }
  base::Status Poll(std::uint64_t now, trading::TradingDispatch& out,
                    bool& event) noexcept override {
    ++poll_calls;
    stage_ = "trading_poll";
    if (!poll_result.ok()) return poll_result;
    if (!has_ready_event(now)) {
      event = false;
      return {};
    }
    const auto& next = events[next_event_];
    const auto id = next.dispatch.update.order.client_order_id.value;
    if (id == 0 || id > orders_.size()) return {base::ErrorCode::kCorrupt, {}};
    orders_[id - 1] = next.dispatch.update.order;
    if (next.update_account) account_ = next.account;
    out = next.dispatch;
    ++next_event_;
    ++delivered;
    event = true;
    return {};
  }

 private:
  md::InstrumentScope instruments_;
  trading::AccountView account_;
  std::vector<model::OrderSnapshot> orders_;
  std::vector<bool> attempted_;
  std::size_t next_event_ = 0;
  bool initialized_ = false;
  bool accepting_ = false;
  bool stopping_ = false;
  const char* stage_ = "";
};
}  // namespace yquant::testing
#endif  // YQUANT_TESTS_MOCK_TRADING_SERVICE_H_
