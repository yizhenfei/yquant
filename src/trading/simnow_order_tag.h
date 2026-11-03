#ifndef YQUANT_SRC_TRADING_SIMNOW_ORDER_TAG_H_
#define YQUANT_SRC_TRADING_SIMNOW_ORDER_TAG_H_

#include <ThostFtdcTraderApi.h>

#include "yquant/trading/simnow_order_history.h"

namespace yquant::trading::internal {

// Identifies the session issuing an action, which can differ from the session
// that originally submitted the order. Fits the SDK action's OrderMemo field.
std::string CancelSessionMemo(int front, int session);

// Writes the caller, sequence and strategy to BusinessUnit and OrderMemo.
// A zero sequence is invalid; failure leaves both SDK fields unchanged.
bool WriteOrderTag(const SimNowOrderOwner& owner, CThostFtdcInputOrderField& row);
// Returns no owner for an absent, incomplete or unrecognized tag.
std::optional<SimNowOrderOwner> ReadOrderTag(const CThostFtdcOrderField& row);

std::optional<SimNowOrderOwner> ReadOrderTag(const CThostFtdcInputOrderField& row);

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_ORDER_TAG_H_
