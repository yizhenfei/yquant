#ifndef YQUANT_SRC_MARKET_SIMNOW_SESSION_H_
#define YQUANT_SRC_MARKET_SIMNOW_SESSION_H_

#include <ThostFtdcMdApi.h>

#include <functional>
#include <memory>

#include "yquant/market/simnow_quote_source.h"

namespace yquant::market::internal {

// Creates the API after validating config. The session calls Release on the
// returned API before destroying its SPI. The factory is not retained.
std::unique_ptr<SimNowQuoteSource> StartSession(
    const SimNowMarketConfig& config,
    const std::function<CThostFtdcMdApi*()>& create);

}  // namespace yquant::market::internal

#endif  // YQUANT_SRC_MARKET_SIMNOW_SESSION_H_
