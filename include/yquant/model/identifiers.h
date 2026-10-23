#ifndef YQUANT_MODEL_IDENTIFIERS_H_
#define YQUANT_MODEL_IDENTIFIERS_H_

// Defines typed identities and canonical venue/symbol encoding shared across
// market data and trading. Instrument metadata is defined in instrument.h.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::model {

// Number of ASCII digits in an equity symbol, excluding the terminator.
inline constexpr std::size_t kSymbolLength = 6;
// Owned canonical symbol storage, including the terminating NUL.
inline constexpr std::size_t kSymbolCapacity = 16;

// Numeric codes are part of the stable instrument encoding. Never renumber
// existing venues when adding another trading venue.
enum class Venue : std::uint8_t {
  kUnknown = 0,
  kSse = 1,
  kSzse = 2,
  kBse = 3,
  kShfe = 4,
  kDce = 5,
  kCzce = 6,
  kCffex = 7,
  kIne = 8,
  kGfex = 9,
};

// Internal instrument encoding constants. Persisted bit layout and radix must
// remain identical across hosts and plugins.
namespace identifier_detail {
// Bit offset of the venue code above the 32-bit instrument code.
inline constexpr unsigned kVenueShift = 32;
inline constexpr std::uint32_t kEquitySymbolMax = 999999;
// Distinguishes futures product/month codes from numeric equity symbols.
inline constexpr std::uint32_t kFuturesMarker = 0x80000000U;
// Reserves the low 14 bits for the encoded contract month.
inline constexpr unsigned kProductShift = 14;
inline constexpr std::uint32_t kMonthMask = 0x3fffU;
// Encodes A..Z as 1..26, leaving zero invalid within a product code.
inline constexpr std::uint32_t kProductRadix = 27;
inline constexpr std::size_t kMaxProductLetters = 3;
// Separates YYMM codes from the directly stored CZCE YMM codes.
inline constexpr std::uint32_t kFourDigitMonthOffset = 1000;
inline constexpr std::uint32_t kMaxThreeDigitMonth = 999;
inline constexpr std::uint32_t kMaxFourDigitMonth = 9999;
// Separates the year prefix from the two-digit month suffix.
inline constexpr std::uint32_t kYearMonthRadix = 100;
inline constexpr std::uint32_t kLastMonth = 12;
inline constexpr std::size_t kThreeDigitMonthLength = 3;
inline constexpr std::size_t kFourDigitMonthLength = 4;
}  // namespace identifier_detail

// Returns the canonical exchange name used in configuration and identity
// parsing, or an empty view for kUnknown and unsupported numeric values.
constexpr std::string_view VenueName(Venue venue) noexcept {
  switch (venue) {
    case Venue::kSse:
      return "SSE";
    case Venue::kSzse:
      return "SZSE";
    case Venue::kBse:
      return "BSE";
    case Venue::kShfe:
      return "SHFE";
    case Venue::kDce:
      return "DCE";
    case Venue::kCzce:
      return "CZCE";
    case Venue::kCffex:
      return "CFFEX";
    case Venue::kIne:
      return "INE";
    case Venue::kGfex:
      return "GFEX";
    case Venue::kUnknown:
      return {};
  }
  return {};
}

// Returns kUnknown for unsupported exchange names; matching is case-sensitive.
constexpr Venue ParseVenue(std::string_view name) noexcept {
  for (auto code = static_cast<std::uint8_t>(Venue::kSse);
       code <= static_cast<std::uint8_t>(Venue::kGfex); ++code) {
    const auto venue = static_cast<Venue>(code);
    if (VenueName(venue) == name) return venue;
  }
  return Venue::kUnknown;
}

// Returns whether `venue` is one of the supported domestic futures venues.
constexpr bool IsFuturesVenue(Venue venue) noexcept {
  return venue >= Venue::kShfe && venue <= Venue::kGfex;
}

// Identifies one configured market-data connection, independently of its
// adapter implementation or the instruments it supplies. Zero is invalid.
struct SourceId {
  std::uint32_t value = 0;

  friend constexpr bool operator==(SourceId, SourceId) noexcept = default;
};

// Stable identity: the upper 32 bits contain the fixed Venue code and the
// lower 32 bits contain either the equity code's numeric value or a marked,
// reversible product/month encoding for futures. Zero is invalid.
// This ID is independent of source, configuration order, and process lifetime.
struct InstrumentId {
  std::uint64_t value = 0;

  friend constexpr bool operator==(InstrumentId,
                                   InstrumentId) noexcept = default;
};

// Identifies a strategy submission within one registered strategy session.
// Assigned monotonically by the strategy host; zero is invalid. Interpret it
// with the durable strategy session identity, not as a gateway ledger ID.
struct StrategyOrderId {
  std::uint64_t value = 0;
  friend constexpr bool operator==(StrategyOrderId,
                                   StrategyOrderId) noexcept = default;
};

// Identifies an order in the tradingd ledger, assigned by TradingEngine. Zero
// is invalid; values increase without reuse within a ledger. Persistent
// gateways reserve ranges before use so restart may skip IDs. Pair with the
// ledger ID when referring to orders across accounts or independent ledger
// histories.
struct ClientOrderId {
  std::uint64_t value = 0;
  friend constexpr bool operator==(ClientOrderId,
                                   ClientOrderId) noexcept = default;
};

// Identifies one locally accepted cancel attempt; zero means no attempt.
struct CancelRequestId {
  std::uint64_t value = 0;
  friend constexpr bool operator==(CancelRequestId,
                                   CancelRequestId) noexcept = default;
};

// Owns up to 64 bytes of opaque trading identity; default construction is
// empty. Tag separates adapter-order and execution IDs at compile time.
// Embedded NUL bytes are preserved; compare views rather than treating them as
// C strings. The adapter or persistence namespace determines identity scope.
template <typename Tag>
class OpaqueTradingId {
 public:
  // Copies 1..64 bytes, retaining no view. Failure preserves the previous ID.
  [[nodiscard]] base::ErrorCode Assign(std::string_view value) noexcept {
    if (value.empty() || value.size() > sizeof(bytes_)) {
      return base::ErrorCode::kInvalidArgument;
    }
    std::memmove(bytes_, value.data(), value.size());
    size_ = static_cast<std::uint8_t>(value.size());
    return base::ErrorCode::kOk;
  }
  // Returns a borrowed byte view, invalidated by destruction or assignment.
  std::string_view view() const noexcept { return {bytes_, size_}; }
  bool empty() const noexcept { return size_ == 0; }
  friend bool operator==(const OpaqueTradingId& lhs,
                         const OpaqueTradingId& rhs) noexcept {
    return lhs.view() == rhs.view();
  }

 private:
  char bytes_[64] = {};
  std::uint8_t size_ = 0;
};

struct AdapterOrderIdTag;
struct ExecutionIdTag;
// Stable adapter-provided order identity; empty until supplied by the adapter.
// May encode a native broker ID or a composite identity; consumers do not parse
// it. Repeated reports and recovery must preserve the same identity.
using AdapterOrderId = OpaqueTradingId<AdapterOrderIdTag>;
// Adapter-provided identity of one execution, unique within its order and
// adapter namespace. Repeated reports and recovery preserve the same value.
using ExecutionId = OpaqueTradingId<ExecutionIdTag>;

// Returns whether an ID has a supported venue and valid symbol encoding,
// without checking whether the instrument is configured or listed.
// Futures use bit 31 of the low word, a base-27 product in bits 14..28,
// and a month code in bits 0..13. Every product digit is 1..26 (A..Z).
// CZCE uses YMM directly; the other futures venues use 1000 + YYMM.
constexpr bool IsValidInstrumentId(InstrumentId id) noexcept {
  const auto venue = id.value >> identifier_detail::kVenueShift;
  const auto code = static_cast<std::uint32_t>(id.value);
  if (venue >= static_cast<unsigned>(Venue::kSse) &&
      venue <= static_cast<unsigned>(Venue::kBse))
    return code <= identifier_detail::kEquitySymbolMax;
  if (venue < static_cast<unsigned>(Venue::kShfe) ||
      venue > static_cast<unsigned>(Venue::kGfex) ||
      (code & identifier_detail::kFuturesMarker) == 0)
    return false;
  auto product = (code & ~identifier_detail::kFuturesMarker) >>
                 identifier_detail::kProductShift;
  if (product == 0) return false;
  unsigned letters = 0;
  while (product != 0) {
    if (product % identifier_detail::kProductRadix == 0 ||
        ++letters > identifier_detail::kMaxProductLetters)
      return false;
    product /= identifier_detail::kProductRadix;
  }
  auto month = code & identifier_detail::kMonthMask;
  if (venue == static_cast<std::uint64_t>(Venue::kCzce)) {
    if (month > identifier_detail::kMaxThreeDigitMonth) return false;
  } else {
    if (month < identifier_detail::kFourDigitMonthOffset ||
        month > identifier_detail::kFourDigitMonthOffset +
                    identifier_detail::kMaxFourDigitMonth)
      return false;
    month -= identifier_detail::kFourDigitMonthOffset;
  }
  return month % identifier_detail::kYearMonthRadix >= 1 &&
         month % identifier_detail::kYearMonthRadix <=
             identifier_detail::kLastMonth;
}

// Builds a stable ID from a canonical venue name and symbol.
// Equity venues accept six ASCII digits, preserving their existing encoding.
// Futures accept 1..3 product letters followed by YMM (CZCE) or YYMM (others),
// with month 01..12. Product letters must be uppercase on CZCE/CFFEX and
// lowercase on SHFE/DCE/INE/GFEX. No hashing or case folding is performed.
// Success copies the identity into out; failure returns kInvalidArgument and
// preserves out. Input views are borrowed only during this call.
[[nodiscard]] inline base::ErrorCode MakeInstrumentId(
    std::string_view venue, std::string_view symbol,
    InstrumentId& out) noexcept {
  const auto parsed_venue = ParseVenue(venue);
  if (parsed_venue == Venue::kUnknown) {
    return base::ErrorCode::kInvalidArgument;
  }
  std::uint32_t symbol_value = 0;
  std::size_t digit_offset = 0;
  if (IsFuturesVenue(parsed_venue)) {
    const auto month_size = parsed_venue == Venue::kCzce
                                ? identifier_detail::kThreeDigitMonthLength
                                : identifier_detail::kFourDigitMonthLength;
    if (symbol.size() <= month_size ||
        symbol.size() > month_size + identifier_detail::kMaxProductLetters) {
      return base::ErrorCode::kInvalidArgument;
    }
    digit_offset = symbol.size() - month_size;
    const char first =
        parsed_venue == Venue::kCzce || parsed_venue == Venue::kCffex ? 'A'
                                                                      : 'a';
    std::uint32_t product = 0;
    for (const char letter : symbol.substr(0, digit_offset)) {
      if (letter < first ||
          letter >=
              first + static_cast<int>(identifier_detail::kProductRadix) - 1) {
        return base::ErrorCode::kInvalidArgument;
      }
      product = product * identifier_detail::kProductRadix +
                static_cast<std::uint32_t>(letter - first + 1);
    }
    symbol_value = identifier_detail::kFuturesMarker |
                   (product << identifier_detail::kProductShift);
  } else if (symbol.size() != kSymbolLength) {
    return base::ErrorCode::kInvalidArgument;
  }
  std::uint32_t number = 0;
  for (const char digit : symbol.substr(digit_offset)) {
    if (digit < '0' || digit > '9') {
      return base::ErrorCode::kInvalidArgument;
    }
    number = number * 10 + static_cast<std::uint32_t>(digit - '0');
  }
  if (IsFuturesVenue(parsed_venue)) {
    if (number % identifier_detail::kYearMonthRadix < 1 ||
        number % identifier_detail::kYearMonthRadix >
            identifier_detail::kLastMonth) {
      return base::ErrorCode::kInvalidArgument;
    }
    if (parsed_venue != Venue::kCzce)
      number += identifier_detail::kFourDigitMonthOffset;
  }
  symbol_value |= number;
  out.value = (static_cast<std::uint64_t>(parsed_venue)
               << identifier_detail::kVenueShift) |
              symbol_value;
  return base::ErrorCode::kOk;
}

}  // namespace yquant::model

#endif  // YQUANT_MODEL_IDENTIFIERS_H_
