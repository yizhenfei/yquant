#include "yquant/trading/internal/order_state.h"

#include <cstdint>

#include "yquant/trading/order.h"

namespace yquant::trading::internal {

bool IsActiveOrder(OrderState state) {
  return state == OrderState::kSent || state == OrderState::kAccepted ||
         state == OrderState::kPartiallyFilled;
}

OrderState StateAfterFill(OrderState state, std::int32_t filled,
                          std::int32_t total) {
  if (filled == total) {
    return OrderState::kFilled;
  }
  if (filled > 0 && state != OrderState::kCanceled) {
    return OrderState::kPartiallyFilled;
  }
  return state;
}

}  // namespace yquant::trading::internal
