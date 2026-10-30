#ifndef YQUANT_MONOD_TRADING_DEMO_H_
#define YQUANT_MONOD_TRADING_DEMO_H_

namespace yquant::monod {

// Runs submission and optional cancellation scenarios; returns 0 on success.
int RunTradingDemo(bool cancel_orders = false);

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_TRADING_DEMO_H_
