#ifndef YQUANT_MODEL_INSTRUMENT_H_
#define YQUANT_MODEL_INSTRUMENT_H_

#include <cstdint>
#include <string_view>
#include <type_traits>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Owns the immutable units and identity shared by all sources of an instrument.
// Hosts publish definitions only after validating the ID, venue, six ASCII
// digits, and price_decimals in 0..6. Quantities are measured in shares.
struct InstrumentDefinition {
  InstrumentId id;
  Venue venue = Venue::kUnknown;
  char symbol[6] = {};  // Exact code, including leading zeros; no terminator.
  // An encoded price p represents p * 10^-price_decimals yuan.
  std::uint8_t price_decimals = 0;

  std::string_view symbol_view() const noexcept {
    return {symbol, sizeof(symbol)};
  }

  friend constexpr bool operator==(const InstrumentDefinition&,
                                   const InstrumentDefinition&) noexcept =
      default;
};

static_assert(std::is_trivially_copyable_v<InstrumentDefinition>);

}  // namespace yquant::model

#endif  // YQUANT_MODEL_INSTRUMENT_H_
