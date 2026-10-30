#ifndef YQUANT_TRADING_INTERNAL_ORDER_STATE_H_
#define YQUANT_TRADING_INTERNAL_ORDER_STATE_H_

#include <cstdint>

#include "yquant/trading/order.h"

namespace yquant::trading::internal {

// Returns whether an order can still have an outstanding remainder canceled.
bool IsActiveOrder(OrderState state);
// Combines a validated quantity in [0, total] with the current order state.
// A full fill wins over cancellation; a partial fill preserves cancellation.
OrderState StateAfterFill(OrderState state, std::int32_t filled,
                          std::int32_t total);

}  // namespace yquant::trading::internal

#endif  // YQUANT_TRADING_INTERNAL_ORDER_STATE_H_
