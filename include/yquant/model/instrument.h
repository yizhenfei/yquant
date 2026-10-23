#ifndef YQUANT_MODEL_INSTRUMENT_H_
#define YQUANT_MODEL_INSTRUMENT_H_

#include <stddef.h>
#include <stdint.h>

#include <string_view>
#include <type_traits>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Fixed-point precision and margin-rate scale shared by validation and ledgers.
inline constexpr uint8_t kMaxPriceDecimals = 6;
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
  char symbol[kSymbolCapacity] =
      {};  // Owned canonical code, NUL terminated by the host.
  // An encoded price p represents p * 10^-price_decimals yuan.
  uint8_t price_decimals = 0;
  InstrumentType type = InstrumentType::kEquity;
  int64_t price_tick = 1;
  int64_t quantity_step = 1;
  int64_t contract_multiplier = 1;
  int64_t margin_rate_ppm = 0;
  int64_t open_fee_per_lot = 0;
  int64_t close_today_fee_per_lot = 0;
  int64_t close_yesterday_fee_per_lot = 0;

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
