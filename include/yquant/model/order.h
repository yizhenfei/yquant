#ifndef YQUANT_MODEL_ORDER_H_
#define YQUANT_MODEL_ORDER_H_

#include <stdint.h>

#include "yquant/model/futures.h"
#include "yquant/model/identifiers.h"

namespace yquant::model {

enum class OrderSide : uint8_t { kUnknown, kBuy, kSell };
enum class OrderState : uint8_t {
  kPendingNew,
  kWorking,
  kFilled,
  kCanceled,
  kRejected
};

constexpr bool IsTerminalOrderState(OrderState state) noexcept {
  return state == OrderState::kFilled || state == OrderState::kCanceled ||
         state == OrderState::kRejected;
}

// Limit-order request; prices use the shared instrument precision. Equity
// quantity is shares and requires kNone; futures quantity is lots and requires
// an explicit opening or day-specific closing effect.
struct OrderRequest {
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  int64_t limit_price = 0;
  int64_t quantity = 0;
  PositionEffect position_effect = PositionEffect::kNone;
};

// Owned view of the latest local order state. Partial fills remain Working.
// Local request acceptance is distinct from broker acceptance; cancel_pending
// can remain true after Filled until the associated cancel result arrives.
struct OrderSnapshot {
  ClientOrderId client_order_id;
  BrokerOrderId broker_order_id;
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  int64_t limit_price = 0;
  int64_t original_qty = 0;
  int64_t cumulative_filled_qty = 0;
  // Potential future fills; zero for terminal states, otherwise original-cum.
  int64_t leaves_qty = 0;
  OrderState state = OrderState::kPendingNew;
  bool cancel_pending = false;
  CancelRequestId cancel_request_id;
  uint64_t report_sequence = 0;
  // Last accepted broker report time; local operations do not refresh this.
  uint64_t receive_time_ns = 0;
  PositionEffect position_effect = PositionEffect::kNone;
  friend bool operator==(const OrderSnapshot&,
                         const OrderSnapshot&) noexcept = default;
};

}  // namespace yquant::model
#endif  // YQUANT_MODEL_ORDER_H_
