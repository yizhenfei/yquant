#ifndef YQUANT_TRADING_TRADING_ADAPTER_H_
#define YQUANT_TRADING_TRADING_ADAPTER_H_

#include <cstdint>

#include "yquant/trading/order.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

struct SubmitOrderRequest;

// Records transmission status independently of the counter's decision.
enum class SendResult { kSent, kNotSent };
enum class AdapterOrderState { kAccepted, kRejected };

// Carries the original ID even before the counter assigns its own order ID.
struct AdapterOrderReport {
  OrderId order_id;
  AdapterOrderState state = AdapterOrderState::kAccepted;
  // Total filled quantity reported by the counter, not this event's increment.
  // Rejection requires zero; acceptance permits values from zero to order size.
  std::int32_t filled_quantity = 0;
};

// Carries an order ID and the service's cancellation attempt ID to the counter.
struct AdapterCancelOrderRequest {
  OrderId order_id;
  std::uint64_t service_cancel_id = 0;
};

// Preserves both IDs so delayed results cannot complete another attempt.
struct AdapterCancelReport {
  OrderId order_id;
  std::uint64_t service_cancel_id = 0;
  CancelResult result = CancelResult::kCanceled;
};

// Receives reports on the service's business thread.
// Copy report data needed after a callback returns.
class ITradingAdapterCallback {
 public:
  virtual ~ITradingAdapterCallback() = default;
  virtual void OnOrderReport(const AdapterOrderReport& report) = 0;
  virtual void OnCancelReport(const AdapterCancelReport& report) = 0;
  virtual void OnTradeReport(const TradeInfo& trade) = 0;
};

// Connects one account to its counter. All methods use the business thread.
// Stop adapter callbacks before destroying the registered callback object.
class ITradingAdapter {
 public:
  virtual ~ITradingAdapter() = default;
  // Borrows callback for all subsequent deliveries; register before submitting.
  virtual void SetCallback(ITradingAdapterCallback& callback) = 0;
  // Copies parameters needed after return. Must not invoke any registered
  // Callback during this call. Buffer reports produced by the counter and
  // deliver them after SubmitOrder returns, through Run on the business thread.
  virtual SendResult SubmitOrder(const SubmitOrderRequest& request) = 0;
  // Copies retained data and sends one attempt. Must not invoke Callback during
  // this call; deliver cancellation results through subsequent Run calls.
  virtual SendResult CancelOrder(const AdapterCancelOrderRequest& request) = 0;
  // Delivers available work up to an internal batch limit, then returns.
  virtual void Run() = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ADAPTER_H_
