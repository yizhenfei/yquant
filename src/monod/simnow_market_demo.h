#ifndef YQUANT_MONOD_SIMNOW_MARKET_DEMO_H_
#define YQUANT_MONOD_SIMNOW_MARKET_DEMO_H_

#include "yquant/market/simnow_quote_source.h"

namespace yquant::monod {

// Verifies one SimNow subscription. Args: front, symbol, optional quote count
// and deadline in seconds. Returns 0 on success, 1 on failure, 2 on bad input.
int RunSimNowMarket(int argc, const char* const* argv,
    decltype(&market::SimNowQuoteSource::Connect) connect =
        market::SimNowQuoteSource::Connect);

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_SIMNOW_MARKET_DEMO_H_
