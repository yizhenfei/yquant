#ifndef YQUANT_MD_INTERNAL_EVENT_CODEC_H_
#define YQUANT_MD_INTERNAL_EVENT_CODEC_H_

#include <cstdint>
#include <span>

#include "md/internal/result.h"
#include "yquant/md/shm_pubsub.h"

namespace yquant::md::internal {

// Encodes to private fixed-word storage. Failure leaves words unchanged.
// words must fit the configured payload maximum; shared access happens
// elsewhere.
Result<void> EncodeEvent(EventView event, std::span<std::uint64_t> words,
                         std::size_t max_payload_size);
// Validates all lengths, flags and reserved bytes before copying.
// Failure leaves output unchanged. The input is an already validated slot copy.
Result<void> DecodeEvent(std::span<const std::uint64_t> words,
                         std::size_t max_payload_size, EventBuffer& output);

}  // namespace yquant::md::internal

#endif  // YQUANT_MD_INTERNAL_EVENT_CODEC_H_
