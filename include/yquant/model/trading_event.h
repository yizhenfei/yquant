#ifndef YQUANT_MODEL_TRADING_EVENT_H_
#define YQUANT_MODEL_TRADING_EVENT_H_

// Defines normalized trading events, order-change reports, execution
// increments and owned diagnostics. Current order views are in order.h.

#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/model/order.h"

namespace yquant::model {

// Describes the event producing an OrderUpdate, rather than its final state.
enum class OrderUpdateKind : std::uint8_t {
  // Broker accepted the order, rather than merely a local submission.
  kAccepted,
  // New order rejected; distinct from rejection of a cancel attempt.
  kRejected,
  kFill,
  kCanceled,
  kCancelRejected,
  // Local shutdown canceled an order before any attempt to send it.
  kCanceledBeforeSend
};

// Owns a bounded diagnostic so it survives reuse of an adapter buffer.
struct OrderReason {
  // Diagnostic code supplied by the producer; interpreted with its text.
  std::int32_t code = 0;
  char text[129] = {};  // 128 payload bytes plus the terminator.
  bool truncated = false;
  // Number of stored message bytes, excluding the terminating NUL; at most 128.
  std::uint8_t size = 0;
  // Replaces code and text, copying at most 128 bytes and appending a NUL.
  // Borrows `message` only during this call; embedded NUL bytes are preserved.
  // Sets truncated only when the input exceeds the message capacity.
  void Set(std::int32_t reason_code, std::string_view message) noexcept {
    code = reason_code;
    const auto count =
        message.size() < sizeof(text) ? message.size() : sizeof(text) - 1;
    if (count != 0) std::memmove(text, message.data(), count);
    text[count] = '\0';
    size = static_cast<std::uint8_t>(count);
    truncated = count != message.size();
  }
  // Returns stored message bytes without the appended terminator. The view
  // borrows this object and may change after Set() or direct text modification.
  std::string_view view() const noexcept {
    const auto count = size <= 128 ? size : 128;
    return {text, static_cast<std::size_t>(count)};
  }
  friend bool operator==(const OrderReason& lhs,
                         const OrderReason& rhs) noexcept {
    return lhs.code == rhs.code && lhs.view() == rhs.view() &&
           lhs.truncated == rhs.truncated;
  }
};

// Owns the order snapshot at callback entry. Later requests may change the
// current order but do not update this historical copy. Use Fill for execution
// increments; cumulative quantities here must not be booked a second time.
struct OrderUpdate {
  OrderSnapshot order;
  OrderUpdateKind kind = OrderUpdateKind::kAccepted;
  // Cancel attempt associated with this event, not necessarily the most recent
  // attempt in order. Zero for non-cancel events or unsolicited broker cancels.
  CancelRequestId cancel_request_id;
  OrderReason reason;
};

// One unique execution; cumulative quantities in OrderUpdate only verify this
// increment. Adapter IDs belong to the configured adapter namespace.
struct Fill {
  ClientOrderId client_order_id;
  // Strategy routing identity; zero in core ledger records.
  StrategyOrderId strategy_order_id;
  AdapterOrderId adapter_order_id;
  ExecutionId execution_id;
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  // Execution price in InstrumentDefinition::price_decimals.
  std::int64_t price = 0;
  // Incremental execution quantity in shares or lots, not a cumulative total.
  std::int64_t quantity = 0;
  // UTC nanoseconds since the Unix epoch; meaningful only if has_event_time.
  std::uint64_t event_time_ns = 0;
  // Local monotonic receive time in the host clock domain.
  std::uint64_t receive_time_ns = 0;
  bool has_event_time = false;
  PositionEffect position_effect = PositionEffect::kNone;
};

// Owns a normalized broker report for trading consumers. Sequence starts at
// one for each order; non-fill events leave fill empty. No field borrows SDK
// data.
struct TradingEvent {
  ClientOrderId client_order_id;
  AdapterOrderId adapter_order_id;
  std::uint64_t report_sequence = 0;
  OrderUpdateKind kind = OrderUpdateKind::kAccepted;
  std::int64_t cumulative_filled_qty = 0;
  std::uint64_t receive_time_ns = 0;
  Fill fill;
  CancelRequestId cancel_request_id;
  OrderReason reason;
};

}  // namespace yquant::model
#endif  // YQUANT_MODEL_TRADING_EVENT_H_
