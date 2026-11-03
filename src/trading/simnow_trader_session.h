#ifndef YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_
#define YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_

#include <ThostFtdcTraderApi.h>

#include <functional>

#include "yquant/trading/simnow_trading_adapter.h"

namespace yquant::trading::internal {

// Creates the native API after validating config. The session calls Release
// after unregistering its SPI; the factory is used only during this call.
std::unique_ptr<SimNowTradingAdapter> StartTraderSession(
    const SimNowTraderConfig& config,
    const std::function<CThostFtdcTraderApi*()>& create);

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_TRADER_SESSION_H_
