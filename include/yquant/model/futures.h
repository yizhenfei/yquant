#ifndef YQUANT_MODEL_FUTURES_H_
#define YQUANT_MODEL_FUTURES_H_

// Defines account mode, opening/closing intent and futures account views.
// Cash-equity account views are in account.h; order requests are in order.h.

#include <stdint.h>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Selects cash-equity accounting or futures margin accounting.
enum class AccountType : uint8_t { kCash, kFutures };

// Determines how a fill changes inventory together with the buy/sell side.
// Buying opens longs or closes shorts; selling opens shorts or closes longs.
enum class PositionEffect : uint8_t {
  // Used for equities; invalid for futures orders.
  kNone,
  kOpen,
  // Closes inventory opened on the current trading day.
  kCloseToday,
  // Closes inventory carried from earlier trading days.
  kCloseYesterday,
  // Broker-managed non-SHFE/INE closing; allocation between trading-day buckets
  // follows that venue's rules and is reported by the authoritative broker.
  kClose = 4
};

// Holds one direction and trading-day bucket in lots. Published fields are
// nonnegative, with frozen_qty <= quantity. Frozen lots are included in
// quantity and cannot be reserved by another closing order.
struct FuturesPositionBucket {
  int64_t quantity = 0;
  int64_t frozen_qty = 0;
  // Total opening cost in micro-yuan, not an average price: remaining FIFO
  // cost in simulation, or the broker-reported cost for a managed account.
  int64_t open_notional = 0;
  friend bool operator==(const FuturesPositionBucket&,
                         const FuturesPositionBucket&) noexcept = default;
};

// Owns futures inventory split by direction and trading day. Today/yesterday
// refer to trading days, not calendar dates. Published quantities have a
// representable gross sum; net zero can still include both longs and shorts.
// Monetary fields use micro-yuan (1 yuan = 1,000,000 units).
struct FuturesPositionSnapshot {
  InstrumentId instrument_id;
  FuturesPositionBucket long_today;
  FuturesPositionBucket long_yesterday;
  FuturesPositionBucket short_today;
  FuturesPositionBucket short_yesterday;
  int64_t margin = 0;
  int64_t unrealized_pnl = 0;
  // Broker-reported closing freezes that cannot be allocated between today's
  // and yesterday's bucket. They exclude all bucket frozen_qty values. Each
  // direction's bucket freezes plus this value may not exceed its holdings.
  // Simulated FIFO accounts always leave both fields zero.
  int64_t unallocated_long_frozen = 0;
  int64_t unallocated_short_frozen = 0;

  // Returns long lots minus short lots, without subtracting frozen lots.
  int64_t net_qty() const noexcept {
    return (long_today.quantity + long_yesterday.quantity) -
           (short_today.quantity + short_yesterday.quantity);
  }
  friend bool operator==(const FuturesPositionSnapshot&,
                         const FuturesPositionSnapshot&) noexcept = default;
};

// Owns a futures account valuation in micro-yuan. Simulation uses gross margin
// without long/short offsets and satisfies equity = balance + unrealized_pnl
// and available = equity - margin - reserved. Broker-managed accounts retain
// reported available, margin and freezes, without enforcing that equation.
// Mark losses may make equity and available negative.
struct FuturesAccountSnapshot {
  // Simulation: cash including realized PnL minus fees. Broker-managed:
  // reported equity minus unrealized PnL.
  int64_t balance = 0;
  // Total marked account value; CTP Balance for broker-managed accounts.
  int64_t equity = 0;
  int64_t available = 0;
  int64_t margin = 0;
  // Pending-order freezes; sum of CTP FrozenMargin, FrozenCash and
  // FrozenCommission for broker-managed accounts.
  int64_t reserved = 0;
  int64_t realized_pnl = 0;
  // Marked position PnL; CTP PositionProfit for broker-managed accounts.
  int64_t unrealized_pnl = 0;
  int64_t commission = 0;
  friend bool operator==(const FuturesAccountSnapshot&,
                         const FuturesAccountSnapshot&) noexcept = default;
};

}  // namespace yquant::model

#endif  // YQUANT_MODEL_FUTURES_H_
