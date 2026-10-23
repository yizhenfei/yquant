#ifndef YQUANT_MODEL_ACCOUNT_H_
#define YQUANT_MODEL_ACCOUNT_H_

// Defines cash-equity balance and position views shared by trading services
// and strategies. Futures margin and inventory views are in futures.h.

#include <stdint.h>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Owns a cash-equity account balance in micro-yuan (1 yuan = 1,000,000 units).
// Buy-order reservations are included in balance; available == balance -
// reserved. Published values are nonnegative. Local reservations change
// available and reserved; confirmed trades change balance.
struct CashSnapshot {
  int64_t balance = 0;
  int64_t reserved = 0;
  int64_t available = 0;
  friend bool operator==(const CashSnapshot&,
                         const CashSnapshot&) noexcept = default;
};

// Owns the cash-equity position for one instrument, measured in shares.
// Sell reservations are included in total_qty and excluded from available_qty.
// Newly bought shares increase total_qty but cannot be sold during this run;
// total_qty can therefore exceed available_qty + reserved_qty. Published
// quantities are nonnegative.
struct PositionSnapshot {
  InstrumentId instrument_id;
  int64_t total_qty = 0;
  int64_t available_qty = 0;
  int64_t reserved_qty = 0;
  friend bool operator==(const PositionSnapshot&,
                         const PositionSnapshot&) noexcept = default;
};

}  // namespace yquant::model

#endif  // YQUANT_MODEL_ACCOUNT_H_
