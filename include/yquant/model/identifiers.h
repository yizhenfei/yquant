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
  if (symbol.size() != 6) {
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
