#ifndef YQUANT_TRADING_TRADING_DISPATCH_H_
#define YQUANT_TRADING_TRADING_DISPATCH_H_

#include "yquant/model/trading_event.h"

namespace yquant::trading {

// Fully committed values owned by the host until serial callback dispatch.
struct TradingDispatch {
  model::OrderUpdate update;
  model::Fill fill;
  bool notify_order = false;
  bool has_fill = false;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_DISPATCH_H_
