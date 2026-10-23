#ifndef YQUANT_MODEL_ORDER_H_
#define YQUANT_MODEL_ORDER_H_

// Defines limit-order requests and current order views shared by strategies
// and trading services. Broker reports and executions are in trading_event.h.

#include <cstdint>

#include "yquant/model/futures.h"
#include "yquant/model/identifiers.h"

namespace yquant::model {

// Describes the trade direction; PositionEffect determines opening or closing.
enum class OrderSide : std::uint8_t { kUnknown, kBuy, kSell };

// Describes the order lifecycle independently of pending cancel attempts.
enum class OrderState : std::uint8_t {
  // Locally accepted, awaiting broker acceptance; may still be queued to send.
  kPendingNew,
  // Broker accepted or partially filled, with quantity still outstanding.
  kWorking,
  kFilled,
  // Remaining quantity canceled; earlier partial fills remain effective.
  kCanceled,
  kRejected
};

// Returns whether no further fills are expected. A terminal order may still
// have an unresolved cancel request.
constexpr bool IsTerminalOrderState(OrderState state) noexcept {
  return state == OrderState::kFilled || state == OrderState::kCanceled ||
         state == OrderState::kRejected;
}

// Requests a limit order for a configured instrument. Price and quantity must
// be positive multiples of its price_tick and quantity_step. Prices use
// InstrumentDefinition::price_decimals; quantities are shares or futures lots.
// Equities require kNone. Simulated futures require kOpen or day-specific
// closing; broker-managed futures use closing effects supported by the venue.
struct OrderRequest {
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  std::int64_t limit_price = 0;
  std::int64_t quantity = 0;
  PositionEffect position_effect = PositionEffect::kNone;
};

// Owns a copyable order view for queries and reports. Instrument, side, price,
// original quantity and position effect retain the original request values.
// Units match OrderRequest. Partial fills remain kWorking; local request
// acceptance does not imply broker acceptance.
struct OrderSnapshot {
  ClientOrderId client_order_id;
  // Empty until a broker identifier has been obtained.
  BrokerOrderId broker_order_id;
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  std::int64_t limit_price = 0;
  std::int64_t original_qty = 0;
  std::int64_t cumulative_filled_qty = 0;
  // Potential future fills; zero for terminal states, otherwise original-cum.
  std::int64_t leaves_qty = 0;
  OrderState state = OrderState::kPendingNew;
  // Remains true until the cancel result arrives, even after a full fill.
  bool cancel_pending = false;
  // Most recent locally accepted cancel attempt; zero if none.
  CancelRequestId cancel_request_id;
  // Last accepted normalized broker report sequence; zero before any report.
  std::uint64_t report_sequence = 0;
  // Monotonic receive time of the last accepted broker report, in the host
  // clock domain; zero before any report. Local operations do not refresh it.
  std::uint64_t receive_time_ns = 0;
  PositionEffect position_effect = PositionEffect::kNone;
  friend bool operator==(const OrderSnapshot&,
                         const OrderSnapshot&) noexcept = default;
};

}  // namespace yquant::model
#endif  // YQUANT_MODEL_ORDER_H_
