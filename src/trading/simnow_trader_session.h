#ifndef YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_
#define YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_

#include <ThostFtdcTraderApi.h>

#include <functional>

#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::trading::internal {

// Creates a trading session using an injectable SDK factory for testing.
// Returns nullptr if configuration is invalid or SDK creation fails.
// The session releases the SDK after unregistering its callbacks.
std::unique_ptr<SimNowTradingAdapter> StartTraderSession(
    const SimNowTraderConfig& config,
    const std::function<CThostFtdcTraderApi*()>& create);

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_
