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
  kCloseYesterday
};

// One direction/day bucket in lots. Open notional is the remaining FIFO cost
// in micro-yuan, not a rounded average price. Frozen lots cannot be closed by
// another order. All fields are nonnegative in published snapshots.
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

  std::int64_t net_qty() const noexcept {
    return (long_today.quantity + long_yesterday.quantity) -
           (short_today.quantity + short_yesterday.quantity);
  }
  friend bool operator==(const FuturesPositionSnapshot&,
                         const FuturesPositionSnapshot&) noexcept = default;
};

// Simulated futures valuation in micro-yuan. Balance includes realized PnL
// minus fees, equity = balance + unrealized_pnl, and available = equity -
// margin - reserved. Mark losses may make equity/available negative; confirmed
// fills are still committed. Margin uses gross exposure without long/short
// offsets. This does not model a broker's settlement or exact margin engine.
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
