#ifndef YQUANT_TRADING_FUNDS_H_
#define YQUANT_TRADING_FUNDS_H_

#include <cstdint>

#include "yquant/model/currency.h"

namespace yquant::trading {

// Contains a complete account balance in the stated currency.
// Amounts use that currency's units. balance is account equity, available is
// available funds, margin is occupied margin, and frozen_margin is reserved
// margin. Finite signed amounts are preserved as reported by the counter.
struct FundsInfo {
  model::Currency currency;
  double balance = 0;
  double available = 0;
  double margin = 0;
  double frozen_margin = 0;
  bool operator==(const FundsInfo&) const = default;
};

// Owns the counter funds and local reservation total at one query time.
// refresh_id identifies the counter snapshot in the bound account's runtime.
struct FundsSnapshot {
  FundsInfo funds;
  std::uint64_t refresh_id = 0;
  // Local order reservations, separate from the counter's raw funds fields.
  double reserved = 0;
  // Returns the counter's available funds less the local reservation total.
  double available_for_orders() const { return funds.available - reserved; }
  bool operator==(const FundsSnapshot&) const = default;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_FUNDS_H_
