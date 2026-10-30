#ifndef YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_
#define YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include "yquant/trading/order.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Selects the next submission's send result and report, delivered through Run.
// Send result and report may deliberately contradict each other.
struct SimulatedSubmit {
  SendResult send_result = SendResult::kSent;
  std::optional<AdapterOrderState> report = AdapterOrderState::kAccepted;
};

// Selects one cancellation's transmission and delayed counter result.
struct SimulatedCancel {
  SendResult send_result = SendResult::kSent;
  std::optional<CancelResult> report = CancelResult::kCanceled;
};

// Saves one simulated account's orders and delivers controlled counter reports.
// Configure and run it on the business thread. Keep the callback alive until
// this adapter stops delivering reports.
class SimulatedTradingAdapter final : public ITradingAdapter {
 public:
  SimulatedTradingAdapter();
  SimulatedTradingAdapter(const SimulatedTradingAdapter&) = delete;
  SimulatedTradingAdapter& operator=(const SimulatedTradingAdapter&) = delete;

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override;
  SendResult SubmitOrder(const SubmitOrderRequest& request) override;
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override;
  void Run() override;

  // Applies behavior to the next submission only, then restores the default.
  void SetNextSubmit(const SimulatedSubmit& behavior) { next_ = behavior; }
  // Applies behavior to the next cancellation only, then restores the default.
  void SetNextCancel(const SimulatedCancel& behavior) {
    next_cancel_ = behavior;
  }
  // Delivers a controlled event directly; also supports malformed-event tests.
  void Emit(const AdapterOrderReport& report);
  // Injects a cancellation event after the request call has returned.
  void EmitCancel(const AdapterCancelReport& report);
  // Returns the saved order, or nullptr if absent. The pointer remains valid
  // until this adapter is destroyed; later submissions do not invalidate it.
  const OrderInfo* FindOrder(const OrderId& id) const;

 private:
  // Preserves native simulated state independently of the service's record.
  struct SimulatedOrder {
    OrderInfo order;
    OrderState state = OrderState::kSent;
  };
  ITradingAdapterCallback* callback_ = nullptr;
  SimulatedSubmit next_;
  SimulatedCancel next_cancel_;
  std::unordered_map<OrderId, SimulatedOrder, OrderIdHash> orders_;
  std::vector<AdapterOrderReport> pending_;
  std::vector<AdapterCancelReport> pending_cancels_;
  std::size_t head_ = 0;
  std::size_t cancel_head_ = 0;
  bool running_ = false;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_
