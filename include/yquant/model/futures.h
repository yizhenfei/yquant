#ifndef YQUANT_MODEL_FUTURES_H_
#define YQUANT_MODEL_FUTURES_H_

#include <cstdint>

#include "yquant/model/identifiers.h"

namespace yquant::model {

enum class AccountType : std::uint8_t { kCash, kFutures };
enum class PositionEffect : std::uint8_t {
  kNone,
  kOpen,
  kCloseToday,
  kCloseYesterday,
  // Broker-managed non-SHFE/INE closing; allocation between trading-day buckets
  // follows that venue's rules and is reported by the authoritative broker.
  kClose = 4
};

// One direction/day bucket in lots. open_notional is cost in micro-yuan:
// remaining FIFO cost for simulation, or the broker's reported cost for a
// broker-managed account. It is never a rounded average price. Frozen lots
// cannot be closed by another order. Published fields are nonnegative.
struct FuturesPositionBucket {
  std::int64_t quantity = 0;
  std::int64_t frozen_qty = 0;
  std::int64_t open_notional = 0;
  friend bool operator==(const FuturesPositionBucket&,
                         const FuturesPositionBucket&) noexcept = default;
};

// Owned futures inventory. Published quantities have a representable gross
// sum; net zero can still mean simultaneous long and short exposure.
struct FuturesPositionSnapshot {
  InstrumentId instrument_id;
  FuturesPositionBucket long_today;
  FuturesPositionBucket long_yesterday;
  FuturesPositionBucket short_today;
  FuturesPositionBucket short_yesterday;
  std::int64_t margin = 0;
  std::int64_t unrealized_pnl = 0;
  // Broker-reported closing freezes that cannot be allocated between today's
  // and yesterday's bucket. They exclude all bucket frozen_qty values. Each
  // direction's bucket freezes plus this value may not exceed its holdings.
  // Simulated FIFO accounts always leave both fields zero.
  std::int64_t unallocated_long_frozen = 0;
  std::int64_t unallocated_short_frozen = 0;

  std::int64_t net_qty() const noexcept {
    return (long_today.quantity + long_yesterday.quantity) -
           (short_today.quantity + short_yesterday.quantity);
  }
  friend bool operator==(const FuturesPositionSnapshot&,
                         const FuturesPositionSnapshot&) noexcept = default;
};

// Futures valuation in micro-yuan. For simulated accounts, balance includes
// realized PnL minus fees, equity = balance + unrealized_pnl, and available =
// equity - margin - reserved. Mark losses may make equity/available negative;
// confirmed fills are still committed. Margin uses gross exposure without
// long/short offsets. Broker-managed accounts preserve reported
// available/margin/freezes: equity is CTP Balance, unrealized_pnl is
// PositionProfit, balance is their difference, and reserved sums
// FrozenMargin/FrozenCash/FrozenCommission. Broker available is not forced to
// satisfy the simulated accounting equation.
struct FuturesAccountSnapshot {
  std::int64_t balance = 0;
  std::int64_t equity = 0;
  std::int64_t available = 0;
  std::int64_t margin = 0;
  std::int64_t reserved = 0;
  std::int64_t realized_pnl = 0;
  std::int64_t unrealized_pnl = 0;
  std::int64_t commission = 0;
  friend bool operator==(const FuturesAccountSnapshot&,
                         const FuturesAccountSnapshot&) noexcept = default;
};

}  // namespace yquant::model

#endif  // YQUANT_MODEL_FUTURES_H_
