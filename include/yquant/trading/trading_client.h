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
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_CLIENT_H_
