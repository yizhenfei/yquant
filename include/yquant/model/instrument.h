#ifndef YQUANT_MODEL_INSTRUMENT_H_
#define YQUANT_MODEL_INSTRUMENT_H_

// Defines immutable instrument metadata and shared price/rate scales.
// Per-source quotes and mutable account state belong in their own models.

#include <stddef.h>
#include <stdint.h>

#include <string_view>
#include <type_traits>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Maximum decimal places supported by fixed-point prices.
inline constexpr uint8_t kMaxPriceDecimals = 6;
// Denominator for margin_rate_ppm; 1,000,000 represents a rate of 100%.
inline constexpr int64_t kMarginRateScale = 1000000;

// Determines the quantity units and accounting model.
enum class InstrumentType : uint8_t { kEquity, kFutures };

// Owns the immutable units and identity shared by all sources of an instrument.
// Hosts validate stable identity, venue-specific symbol and precision 0..6.
// Equity quantities are shares; futures quantities are lots. Futures require
// positive tick, step and multiplier, margin rate 1..1,000,000 and nonnegative
// per-lot fees. Monetary fee fields use micro-yuan (1 yuan = 1,000,000).
struct InstrumentDefinition {
  InstrumentId id;
  Venue venue = Venue::kUnknown;
  // Canonical venue-specific code, NUL terminated by the host.
  char symbol[kSymbolCapacity] = {};
  // An encoded price p represents p * 10^-price_decimals yuan.
  uint8_t price_decimals = 0;
  InstrumentType type = InstrumentType::kEquity;
  // Minimum price increment in the same fixed-point units as prices.
  int64_t price_tick = 1;
  // Minimum order quantity increment, in shares or lots.
  int64_t quantity_step = 1;
  // Multiplies price times lots to obtain futures notional; 1 for equities.
  int64_t contract_multiplier = 1;
  int64_t margin_rate_ppm = 0;
  int64_t open_fee_per_lot = 0;
  int64_t close_today_fee_per_lot = 0;
  int64_t close_yesterday_fee_per_lot = 0;

  // Returns a view up to the first NUL or the storage bound. The view borrows
  // this object and is invalidated by its destruction or symbol modification.
  std::string_view symbol_view() const noexcept {
    size_t size = 0;
    while (size < sizeof(symbol) && symbol[size] != '\0') {
      ++size;
    }
    return {symbol, size};
  }

  friend constexpr bool operator==(const InstrumentDefinition&,
                                   const InstrumentDefinition&) noexcept =
      default;
};

static_assert(std::is_trivially_copyable_v<InstrumentDefinition>);

}  // namespace yquant::model

#endif  // YQUANT_MODEL_INSTRUMENT_H_
