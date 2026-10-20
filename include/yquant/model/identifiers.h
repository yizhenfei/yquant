#ifndef YQUANT_MODEL_IDENTIFIERS_H_
#define YQUANT_MODEL_IDENTIFIERS_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::model {

// Number of ASCII digits in a supported instrument symbol; excludes '\0'.
inline constexpr std::size_t kSymbolLength = 6;

// Numeric codes are part of the stable instrument encoding. Never renumber
// existing venues when adding another trading venue.
enum class Venue : std::uint8_t {
  kUnknown = 0,
  kSse = 1,
  kSzse = 2,
  kBse = 3,
};

// Identifies one configured market-data connection, independently of its
// adapter implementation or the instruments it supplies. Zero is invalid.
struct SourceId {
  std::uint32_t value = 0;

  friend constexpr bool operator==(SourceId, SourceId) noexcept = default;
};

// Stable identity: the upper 32 bits contain the fixed Venue code and the
// lower 32 bits contain the six-digit symbol's numeric value. Zero is invalid.
// This ID is independent of source, configuration order, and process lifetime.
struct InstrumentId {
  std::uint64_t value = 0;

  friend constexpr bool operator==(InstrumentId,
                                   InstrumentId) noexcept = default;
};

// Accepts exactly SSE, SZSE, or BSE and exactly six ASCII digits. Copies the
// encoded identity into out on success; otherwise returns kInvalidArgument
// and preserves out. Borrows the input views only for this call.
[[nodiscard]] inline base::ErrorCode MakeInstrumentId(
    std::string_view venue, std::string_view symbol,
    InstrumentId& out) noexcept {
  Venue parsed_venue;
  if (venue == "SSE") {
    parsed_venue = Venue::kSse;
  } else if (venue == "SZSE") {
    parsed_venue = Venue::kSzse;
  } else if (venue == "BSE") {
    parsed_venue = Venue::kBse;
  } else {
    return base::ErrorCode::kInvalidArgument;
  }
  if (symbol.size() != kSymbolLength) {
    return base::ErrorCode::kInvalidArgument;
  }
  std::uint32_t symbol_value = 0;
  for (const char digit : symbol) {
    if (digit < '0' || digit > '9') {
      return base::ErrorCode::kInvalidArgument;
    }
    symbol_value = symbol_value * 10 + static_cast<std::uint32_t>(digit - '0');
  }
  out.value = (static_cast<std::uint64_t>(parsed_venue) << 32) | symbol_value;
  return base::ErrorCode::kOk;
}

}  // namespace yquant::model

#endif  // YQUANT_MODEL_IDENTIFIERS_H_
