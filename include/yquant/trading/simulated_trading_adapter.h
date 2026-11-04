#ifndef YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_
#define YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "yquant/trading/funds.h"
#include "yquant/trading/order.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trade.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Selects the next submission's return value and the report delivered by Run.
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

// Simulates one account for testing order requests and counter reports.
// Tests control send results and may inject failed, duplicate or late reports.
// Use on the business thread.
class SimulatedTradingAdapter final : public ITradingAdapter {
 public:
  SimulatedTradingAdapter();
  SimulatedTradingAdapter(const SimulatedTradingAdapter&) = delete;
  SimulatedTradingAdapter& operator=(const SimulatedTradingAdapter&) = delete;

  // ITradingAdapter implementation.
  void SetCallback(ITradingAdapterCallback& callback) override;
  bool StartAccountSync(std::uint64_t sync_id) override;
  SendResult SubmitOrder(const SubmitOrderRequest& request) override;
  SendResult CancelOrder(const AdapterCancelOrderRequest& request) override;
  void Run() override;

  // Copies a complete, already reconciled simulated startup baseline. It must
  // include all historical events queued before StartAccountSync. With no
  // configured baseline, tests explicitly queue the requested response events.
  // Active native orders or nonzero freezes prevent successful completion.
  void SetInitialState(const FundsInfo& funds,
                       std::span<const PositionInfo> positions);
  // Queues an initialization outcome; supports failed and malformed responses.
  void QueueAccountSync(const AdapterAccountSyncReport& report);
  std::size_t sync_requests() const { return sync_requests_; }
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
  // Saves one new native fill for an active order and queues its report.
  // Returns false for invalid, duplicate or excessive fills without changes.
  // Delivers the fill report through a subsequent Run.
  bool Fill(const TradeInfo& trade);
  // Injects an event directly, including malformed and late reports, without
  // updating the simulated book. Call only after a request method returns.
  void EmitTrade(const TradeInfo& trade);
  // Copies one controlled funds event for delivery by a subsequent Run.
  // Can be queued before binding; Run waits until a callback is registered.
  void QueueFunds(const AdapterFundsReport& report);
  // Copies a controlled positions event for delivery by a subsequent Run.
  // Events queued before binding wait for a registered callback.
  void QueuePositions(const AdapterPositionsReport& report);
  // Returns the saved order, or nullptr if the ID is unknown.
  const OrderInfo* FindOrder(const OrderId& id) const;

 private:
  // Preserves native simulated state independently of the service's record.
  struct SimulatedOrder {
    OrderInfo order;
    OrderState state = OrderState::kSent;
    std::int32_t filled_quantity = 0;
  };
  // Validates native activity when a queued cancellation reaches the counter.
  void DeliverCancel(AdapterCancelReport report);
  std::optional<FundsInfo> initial_funds_;
  std::vector<PositionInfo> initial_positions_;
  std::size_t sync_requests_ = 0;
  ITradingAdapterCallback* callback_ = nullptr;
  SimulatedSubmit next_;
  SimulatedCancel next_cancel_;
  std::unordered_map<OrderId, SimulatedOrder, OrderIdHash> orders_;
  std::unordered_set<TradeId, TradeIdHash> trades_;
  std::vector<std::variant<AdapterOrderReport, AdapterCancelReport, TradeInfo,
                           AdapterFundsReport, AdapterPositionsReport,
                           AdapterAccountSyncReport>>
      pending_;
  std::size_t head_ = 0;
  bool running_ = false;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMULATED_TRADING_ADAPTER_H_
