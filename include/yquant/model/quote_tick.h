#ifndef YQUANT_MODEL_QUOTE_TICK_H_
#define YQUANT_MODEL_QUOTE_TICK_H_

#include <cstdint>
#include <type_traits>

#include "yquant/model/identifiers.h"

namespace yquant::model {

enum class QuoteFlag : std::uint8_t {
  kBidValid = 1,
  kAskValid = 2,
  kTradable = 4,
  kEventTimeValid = 8,
};

inline constexpr std::uint8_t kKnownQuoteFlags = 0x0f;

constexpr bool HasQuoteFlag(std::uint8_t flags, QuoteFlag flag) noexcept {
  return (flags & static_cast<std::uint8_t>(flag)) != 0;
}

// Self-contained, copyable top-of-book snapshot from one configured source.
// The host validates it before publishing it to a strategy. Sequence numbers
// increase within (source_id, instrument_id), without resetting during a run.
// This is a complete one-level view, not a complete order-book snapshot.
struct QuoteTick {
  SourceId source_id;
  InstrumentId instrument_id;
  std::uint64_t sequence = 0;
  // UTC nanoseconds since the Unix epoch; zero when kEventTimeValid is absent.
  std::uint64_t event_time_ns = 0;
  // Nanoseconds in the same process-relative monotonic domain as Context::Now.
  // Preserved through dispatch so strategies can measure the data's age.
  std::uint64_t receive_time_ns = 0;
  // Prices use InstrumentDefinition::price_decimals; quantities use shares
  // for equities and lots for futures.
  // A missing side has zero price and quantity; a present side has qty > 0.
  std::int64_t bid_price = 0;
  std::int64_t bid_qty = 0;
  std::int64_t ask_price = 0;
  std::int64_t ask_qty = 0;
  std::uint8_t flags = 0;

  friend constexpr bool operator==(const QuoteTick&,
                                   const QuoteTick&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<QuoteTick>);

}  // namespace yquant::model

#endif  // YQUANT_MODEL_QUOTE_TICK_H_
