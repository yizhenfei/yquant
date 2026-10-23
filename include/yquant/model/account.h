#ifndef YQUANT_MODEL_ACCOUNT_H_
#define YQUANT_MODEL_ACCOUNT_H_

#include <stdint.h>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// CNY cash in micro-yuan (1 yuan = 1,000,000 units). Reservations are included
// in balance; available == balance - reserved. All values are nonnegative.
struct CashSnapshot {
  int64_t balance = 0;
  int64_t reserved = 0;
  int64_t available = 0;
  friend bool operator==(const CashSnapshot&,
                         const CashSnapshot&) noexcept = default;
};

// Shares held for one instrument. Sell reservations are excluded from
// available_qty and included in total_qty. Newly bought shares increase total
// but are unavailable for selling during this run.
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
