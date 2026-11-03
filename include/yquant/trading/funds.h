#ifndef YQUANT_TRADING_FUNDS_H_
#define YQUANT_TRADING_FUNDS_H_

#include <cstdint>

#include "yquant/model/currency.h"

namespace yquant::trading {

// Contains a complete account balance in the stated currency.
// Amounts use the stated currency's units. balance excludes unrealized PnL.
// available is cash after occupied margin and before pending-order holds.
// margin is the margin occupied by existing positions.
// frozen_margin is the margin reserved for pending orders, excluding fees.
// The adapter normalizes initial counter values to this book-accounting basis.
struct FundsInfo {
  model::Currency currency;
  double balance = 0;
  double available = 0;
  double margin = 0;
  double frozen_margin = 0;
  bool operator==(const FundsInfo&) const = default;
};

// Owns the local funds value and pending-order reservation at query time.
// refresh_id identifies the initial counter baseline for this runtime.
struct FundsSnapshot {
  FundsInfo funds;
  std::uint64_t refresh_id = 0;
  // Includes the margin and estimated fees of orders not yet booked.
  double reserved = 0;
  // Cumulative booked fees and realized PnL since account initialization.
  double commission = 0;
  double realized_pnl = 0;
  // Returns funds remaining after pending-order reservations.
  double available_for_orders() const { return funds.available - reserved; }
  bool operator==(const FundsSnapshot&) const = default;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_FUNDS_H_
