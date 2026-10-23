#ifndef YQUANT_MODEL_QUOTE_TICK_H_
#define YQUANT_MODEL_QUOTE_TICK_H_

// Defines complete best-bid/ask snapshots and their validity flags for
// publication from market-data sources to strategies.

#include <stdint.h>

#include <type_traits>

#include "yquant/model/identifiers.h"

namespace yquant::model {

// Bit flags for QuoteTick::flags.
// Each enumerator must be a distinct power of two (one bit per flag).
// Use SetQuoteFlag() to set or clear a flag, and HasQuoteFlag() to query it.
enum class QuoteFlag : uint8_t {
  // bid_price and bid_qty describe a present bid, even if bid_price is zero.
  kBidValid = 1,
  // ask_price and ask_qty describe a present ask, even if ask_price is zero.
  kAskValid = 2,
  // The source permits trading. Nontradable snapshots are still delivered.
  kTradable = 4,
  // event_time_ns is available; zero is valid when this bit is set.
  kEventTimeValid = 8,
};

// Mask of supported QuoteTick::flags bits; all other bits must be zero.
inline constexpr uint8_t kKnownQuoteFlags = 0x0f;

// Returns whether the specified QuoteFlag bit is set in flags.
constexpr bool HasQuoteFlag(uint8_t flags, QuoteFlag flag) noexcept {
  return (flags & static_cast<uint8_t>(flag)) != 0;
}

// Sets `flag` in `flags` when enabled, or clears it otherwise. Preserves all
// other bits in the existing mask.
constexpr void SetQuoteFlag(uint8_t& flags, QuoteFlag flag,
                            bool enabled = true) noexcept {
  const auto mask = static_cast<uint8_t>(flag);
  if (enabled) {
    flags |= mask;
  } else {
    flags &= static_cast<uint8_t>(~mask);
  }
}

// Self-contained, copyable top-of-book snapshot from one configured source.
// The host validates it before publishing it to a strategy. Sequence numbers
// increase within (source_id, instrument_id), without resetting during a run.
// Each tick replaces both sides of the previous one-level view. It carries
// no depth beyond the best bid and ask.
struct QuoteTick {
  SourceId source_id;
  InstrumentId instrument_id;
  uint64_t sequence = 0;
  // UTC nanoseconds since the Unix epoch; zero when kEventTimeValid is absent.
  uint64_t event_time_ns = 0;
  // Nanoseconds in the same process-relative monotonic domain as Context::Now.
  // Preserved through dispatch so strategies can measure the data's age.
  uint64_t receive_time_ns = 0;
  // Prices use InstrumentDefinition::price_decimals; quantities use shares
  // for equities and lots for futures.
  // A missing side has zero price and quantity; a present side has qty > 0.
  int64_t bid_price = 0;
  int64_t bid_qty = 0;
  int64_t ask_price = 0;
  int64_t ask_qty = 0;
  // QuoteFlag mask. Side presence is determined by validity bits, not prices;
  // unsupported bits must be zero.
  uint8_t flags = 0;

  friend constexpr bool operator==(const QuoteTick&,
                                   const QuoteTick&) noexcept = default;
};

static_assert(std::is_trivially_copyable_v<QuoteTick>);

}  // namespace yquant::model

#endif  // YQUANT_MODEL_QUOTE_TICK_H_
