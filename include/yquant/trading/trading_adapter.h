#ifndef YQUANT_TRADING_TRADING_ADAPTER_H_
#define YQUANT_TRADING_TRADING_ADAPTER_H_

#include "yquant/trading/order.h"

namespace yquant::trading {

struct SubmitOrderRequest;

// Records transmission status independently of the counter's decision.
enum class SendResult { kSent, kNotSent };
enum class AdapterOrderState { kAccepted, kRejected };

// Carries the original ID even before the counter assigns its own order ID.
struct AdapterOrderReport {
  OrderId order_id;
  AdapterOrderState state = AdapterOrderState::kAccepted;
};

// Receives reports on the service's business thread.
// Copy any report data needed after OnOrderReport returns.
class ITradingAdapterCallback {
 public:
  virtual ~ITradingAdapterCallback() = default;
  virtual void OnOrderReport(const AdapterOrderReport& report) = 0;
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
  // Delivers available work up to an internal batch limit, then returns.
  virtual void Run() = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ADAPTER_H_
