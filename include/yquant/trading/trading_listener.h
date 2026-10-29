#ifndef YQUANT_TRADING_TRADING_LISTENER_H_
#define YQUANT_TRADING_TRADING_LISTENER_H_

namespace yquant::trading {

struct RequestFailure;
struct OrderReport;

// Receives reports on the business thread after request methods return.
// Keep listeners alive for the service's runtime, including terminal orders.
// Copy any reports needed after a callback returns. Callbacks may submit new
// requests; calling Run from a callback does no work.
class TradingListener {
 public:
  virtual ~TradingListener() = default;
  virtual void OnRequestFailure(const RequestFailure& failure) = 0;
  virtual void OnOrderReport(const OrderReport& report) = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_LISTENER_H_
