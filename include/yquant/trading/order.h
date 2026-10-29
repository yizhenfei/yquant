#ifndef YQUANT_TRADING_ORDER_H_
#define YQUANT_TRADING_ORDER_H_

#include <cstddef>
#include <cstdint>
#include <functional>

namespace yquant::trading {

// Identifies an order independently of its account, strategy and storage slot.
struct OrderId {
  std::uint64_t caller_service_id = 0;
  std::uint64_t sequence = 0;
  bool operator==(const OrderId&) const = default;
};

// Hashes both fields so each caller can independently start its sequence at 1.
struct OrderIdHash {
  std::size_t operator()(const OrderId& id) const {
    const auto caller = std::hash<std::uint64_t>{}(id.caller_service_id);
    const auto sequence = std::hash<std::uint64_t>{}(id.sequence);
    return caller ^ (sequence + 0x9e3779b9U + (caller << 6) + (caller >> 2));
  }
};

enum class Side { kBuy, kSell };
enum class Offset { kOpen, kClose, kCloseToday, kCloseYesterday };

// Stores an order's identity, ownership and original parameters.
// Requests and snapshots each own a copy of this value.
struct OrderInfo {
  OrderId order_id;
  std::uint64_t account_id = 0;
  std::uint64_t strategy_id = 0;
  std::uint32_t instrument_id = 0;
  Side side = Side::kBuy;
  Offset offset = Offset::kOpen;
  double price = 0;
  std::int32_t quantity = 0;
  bool operator==(const OrderInfo&) const = default;
};

// Tracks submission and counter acceptance for an established order.
// Request validation failures do not create a new order.
//
// Transitions:
// - kSubmitting -> kSent or kSendFailed on adapter return.
//   The adapter must not invoke callbacks during SubmitOrder.
// - kSent or kSendFailed -> kAccepted or kRejected
//   on a valid counter report. A report after kSendFailed emits a diagnostic.
// - Duplicate reports leave state unchanged. Conflicting acceptance/rejection
//   reports and invalid reports are diagnosed and ignored.
enum class OrderState {
  // Initial state: the service saved the request and listener and is awaiting
  // the adapter's send result.
  kSubmitting,
  // The adapter sent the request; the counter decision is still pending.
  kSent,
  // The counter confirmed acceptance. This does not indicate a fill.
  kAccepted,
  // A valid counter report confirmed rejection.
  kRejected,
  // The adapter reported that it did not send the request.
  // No valid counter report has confirmed otherwise.
  // A later valid report can correct this state.
  kSendFailed,
};

// Owns the original order information and its state at one particular version.
// Copy the snapshot to retain it after a report callback returns.
struct OrderSnapshot {
  OrderInfo order;
  OrderState state = OrderState::kSubmitting;
  std::int32_t filled_quantity = 0;
  std::uint64_t version = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_ORDER_H_
