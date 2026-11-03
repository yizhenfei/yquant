#ifndef YQUANT_MONOD_TRADER_LOGIN_H_
#define YQUANT_MONOD_TRADER_LOGIN_H_

#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::monod {

// Runs a login-only probe for FRONT and optional timeout seconds. Returns 0
// after login, 1 on session failure, or 2 for invalid command-line input.
int RunTraderLogin(int argc, const char* const* argv,
    decltype(&trading::SimNowTradingAdapter::Connect) connect =
        trading::SimNowTradingAdapter::Connect);

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_TRADER_LOGIN_H_
