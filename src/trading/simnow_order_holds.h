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

// Lists one rate query per distinct working-order scope. Unknown states fail
// before any order can be omitted from reconciliation.
std::string CollectOrderRates(const SimNowOrderHistory& history, OrderRates& rates);
// Publishes all verified per-order holds together or sets holds_error.
void ReconcileOrderHolds(SimNowStartupSnapshot& snapshot, const OrderRates& rates);

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_ORDER_HOLDS_H_
