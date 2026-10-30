#ifndef YQUANT_TRADING_TRADING_CLIENT_H_
#define YQUANT_TRADING_TRADING_CLIENT_H_

#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Submits orders on the caller's business thread across deployment locations.
// Keep listeners alive for the target service's runtime. Inputs are borrowed
// until return; reports are borrowed until their callback returns.
class TradingClient {
 public:
  virtual ~TradingClient() = default;
  // kDelivered means the service received the request, including requests that
  // fail validation. Outcomes arrive after return through listener callbacks.
  // kIdExhausted means no delivery and no callback for this invocation.
  virtual RequestResult SubmitOrder(const SubmitOrderRequest& request,
                                    TradingListener& listener) = 0;
  // Uses a new cancel_request_id for each attempt. Delivery and listener
  // lifetime follow SubmitOrder; counter results arrive through OnCancelReport.
  virtual RequestResult CancelOrder(const CancelOrderRequest& request,
                                    TradingListener& listener) = 0;
  // Returns the current snapshot for a matching order, account and strategy.
  // Returns false and leaves snapshot unchanged if no matching state is ready.
  virtual bool QueryOrder(const QueryOrderRequest& query,
                          OrderSnapshot& snapshot) const = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_CLIENT_H_
