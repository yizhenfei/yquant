#ifndef YQUANT_TESTS_SCENARIOS_TRADER_LOGIN_H_
#define YQUANT_TESTS_SCENARIOS_TRADER_LOGIN_H_

#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::verification {

// Runs a read-only probe for FRONT and optional timeout seconds. With
// collect_snapshot, a symbol after the timeout also requests contract terms.
// Returns 0 on success, 1 on session/query failure, or 2 for invalid arguments.
int RunTraderLogin(int argc, const char* const* argv, bool collect_snapshot,
    decltype(&trading::SimNowTradingAdapter::Connect) connect =
        trading::SimNowTradingAdapter::Connect);

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_TRADER_LOGIN_H_
