#ifndef YQUANT_SRC_TRADING_SIMNOW_ORDER_HOLDS_H_
#define YQUANT_SRC_TRADING_SIMNOW_ORDER_HOLDS_H_

#include <map>
#include <string>
#include <tuple>

#include "yquant/trading/simnow_startup_snapshot.h"

namespace yquant::trading::internal {

// Exchange, symbol, investment unit and native hedge category scope each rate.
using OrderRateKey = std::tuple<std::string, std::string, std::string, char>;
using OrderRates = std::map<OrderRateKey, SimNowContractTerms>;

// Adds the rate-query keys needed by working orders. Returns an empty string
// on success or a diagnostic for invalid or unresolved orders; on failure,
// rates may contain partial results and must not be used for reconciliation.
std::string CollectOrderRates(const SimNowOrderHistory& history, OrderRates& rates);
// Sets holds only when per-order calculations match the counter totals.
// On failure, leaves holds empty and sets holds_error.
void ReconcileOrderHolds(SimNowStartupSnapshot& snapshot, const OrderRates& rates);

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_ORDER_HOLDS_H_
