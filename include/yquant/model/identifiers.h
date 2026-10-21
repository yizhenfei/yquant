#ifndef YQUANT_MODEL_IDENTIFIERS_H_
#define YQUANT_MODEL_IDENTIFIERS_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::model {

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

// Canonical exchange names used by configuration and stable identity parsing.
// Unknown numeric values return an empty view.
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

// Process-local order identity. Assigned monotonically by the host, never
// reused during a run; zero is invalid.
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

// Owns an opaque session-scoped identifier. Embedded NUL bytes are preserved;
// callers compare views rather than interpreting these bytes as C strings.
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

struct BrokerOrderIdTag;
struct ExecutionIdTag;
using BrokerOrderId = OpaqueTradingId<BrokerOrderIdTag>;
using ExecutionId = OpaqueTradingId<ExecutionIdTag>;

// Validates a stable identity without instrument metadata or allocation.
// Futures use bit 31 of the low word, a base-27 product in bits 14..28,
// and a month code in bits 0..13. Every product digit is 1..26 (A..Z).
// CZCE uses YMM directly; the other futures venues use 1000 + YYMM.
constexpr bool IsValidInstrumentId(InstrumentId id) noexcept {
  const auto venue = id.value >> 32;
  const auto code = static_cast<std::uint32_t>(id.value);
  if (venue >= 1 && venue <= 3) return code <= 999999;
  if (venue < 4 || venue > 9 || (code & 0x80000000U) == 0) return false;
  auto product = (code & 0x7fffffffU) >> 14;
  if (product == 0) return false;
  unsigned letters = 0;
  while (product != 0) {
    if (product % 27 == 0 || ++letters > 3) return false;
    product /= 27;
  }
  auto month = code & 0x3fffU;
  if (venue == static_cast<std::uint64_t>(Venue::kCzce)) {
    if (month > 999) return false;
  } else {
    if (month < 1000 || month > 10999) return false;
    month -= 1000;
  }
  return month % 100 >= 1 && month % 100 <= 12;
}

// Equity venues accept six ASCII digits, preserving their existing encoding.
// Futures accept 1..3 product letters followed by YMM (CZCE) or YYMM (others),
// with month 01..12. Product letters must be uppercase on CZCE/CFFEX and
// lowercase on SHFE/DCE/INE/GFEX. No hashing or case folding is performed.
// Success copies the identity into out; failure returns kInvalidArgument and
// preserves out. Input views are borrowed only during this call.
[[nodiscard]] inline base::ErrorCode MakeInstrumentId(
    std::string_view venue, std::string_view symbol,
    InstrumentId& out) noexcept {
  Venue parsed_venue = Venue::kUnknown;
  for (std::uint8_t code = 1; code <= 9; ++code) {
    if (VenueName(static_cast<Venue>(code)) == venue) {
      parsed_venue = static_cast<Venue>(code);
      break;
    }
  }
  if (parsed_venue == Venue::kUnknown) {
    return base::ErrorCode::kInvalidArgument;
  }
  std::uint32_t symbol_value = 0;
  std::size_t digit_offset = 0;
  if (IsFuturesVenue(parsed_venue)) {
    const auto month_size = parsed_venue == Venue::kCzce ? 3U : 4U;
    if (symbol.size() <= month_size || symbol.size() > month_size + 3) {
      return base::ErrorCode::kInvalidArgument;
    }
    digit_offset = symbol.size() - month_size;
    const char first =
        parsed_venue == Venue::kCzce || parsed_venue == Venue::kCffex ? 'A'
                                                                      : 'a';
    std::uint32_t product = 0;
    for (const char letter : symbol.substr(0, digit_offset)) {
      if (letter < first || letter > first + 25) {
        return base::ErrorCode::kInvalidArgument;
      }
      product = product * 27 + static_cast<std::uint32_t>(letter - first + 1);
    }
    symbol_value = 0x80000000U | (product << 14);
  } else if (symbol.size() != 6) {
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
    if (number % 100 < 1 || number % 100 > 12) {
      return base::ErrorCode::kInvalidArgument;
    }
    if (parsed_venue != Venue::kCzce) number += 1000;
  }
  symbol_value |= number;
  out.value = (static_cast<std::uint64_t>(parsed_venue) << 32) | symbol_value;
  return base::ErrorCode::kOk;
}

}  // namespace yquant::model

#endif  // YQUANT_MODEL_IDENTIFIERS_H_
