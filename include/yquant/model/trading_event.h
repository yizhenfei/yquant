#ifndef YQUANT_MODEL_TRADING_EVENT_H_
#define YQUANT_MODEL_TRADING_EVENT_H_

#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/model/order.h"

namespace yquant::model {

enum class OrderUpdateKind : std::uint8_t {
  kAccepted,
  kRejected,
  kFill,
  kCanceled,
  kCancelRejected,
  kCanceledBeforeSend
};

// Owned bounded diagnostic, preserved after an adapter buffer is reused.
struct OrderReason {
  std::int32_t code = 0;
  char text[129] = {};  // 128 payload bytes plus the terminator.
  bool truncated = false;
  std::uint8_t size = 0;
  // Copies a terminated diagnostic; oversized input is marked and shortened.
  void Set(std::int32_t reason_code, std::string_view message) noexcept {
    code = reason_code;
    const auto count =
        message.size() < sizeof(text) ? message.size() : sizeof(text) - 1;
    if (count != 0) std::memmove(text, message.data(), count);
    text[count] = '\0';
    size = static_cast<std::uint8_t>(count);
    truncated = count != message.size();
  }
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

// Snapshot at callback entry. Later local requests can change pending flags;
// this historical copy remains unchanged and never adds a fill a second time.
struct OrderUpdate {
  OrderSnapshot order;
  OrderUpdateKind kind = OrderUpdateKind::kAccepted;
  CancelRequestId cancel_request_id;
  OrderReason reason;
};

// One unique execution; cumulative quantities in OrderUpdate only verify this
// increment. Broker IDs belong to the configured adapter session.
struct Fill {
  ClientOrderId client_order_id;
  BrokerOrderId broker_order_id;
  ExecutionId execution_id;
  InstrumentId instrument_id;
  OrderSide side = OrderSide::kUnknown;
  std::int64_t price = 0;
  std::int64_t quantity = 0;
  std::uint64_t event_time_ns = 0;
  std::uint64_t receive_time_ns = 0;
  bool has_event_time = false;
  PositionEffect position_effect = PositionEffect::kNone;
};

}  // namespace yquant::model
#endif  // YQUANT_MODEL_TRADING_EVENT_H_
