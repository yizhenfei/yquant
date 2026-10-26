#ifndef YQUANT_MARKET_INTERNAL_EVENT_CODEC_H_
#define YQUANT_MARKET_INTERNAL_EVENT_CODEC_H_

#include <stddef.h>
#include <stdint.h>

#include <span>

#include "market/internal/result.h"
#include "yquant/market/event.h"

namespace yquant::market::internal {

inline constexpr size_t kWireHeaderSize = 64;
inline constexpr uint64_t kEventSchemaVersion = 1;

// Encodes to private fixed-word storage. Failure leaves words unchanged.
// words must fit the configured payload maximum; shared access happens
// elsewhere.
Result<void> EncodeEvent(EventView event, std::span<uint64_t> words,
                         size_t max_payload_size);
// Validates all lengths, flags and reserved bytes before copying.
// Failure leaves output unchanged. The input is an already validated slot copy.
Result<void> DecodeEvent(std::span<const uint64_t> words,
                         size_t max_payload_size, EventBuffer& output);

}  // namespace yquant::market::internal

#endif  // YQUANT_MARKET_INTERNAL_EVENT_CODEC_H_
