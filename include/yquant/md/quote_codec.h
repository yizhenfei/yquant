#ifndef YQUANT_MD_QUOTE_CODEC_H_
#define YQUANT_MD_QUOTE_CODEC_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "yquant/base/status.h"
#include "yquant/md/shm_pubsub.h"
#include "yquant/model/quote_tick.h"

namespace yquant::md {

inline constexpr std::size_t kQuoteFrameSize = 80;
inline constexpr std::uint32_t kQuoteEventType = 1;
using QuoteFrame = std::array<std::byte, kQuoteFrameSize>;

// Validates a complete quote independently of any process-relative clock.
// On error the input is unchanged. receive_time_ns is deliberately ignored.
[[nodiscard]] base::Status ValidateQuote(
    const model::QuoteTick& quote) noexcept;

// Encodes a padding-free, versioned little-endian frame. receive_time_ns is
// local to the producer process and is not transmitted. Failure preserves out.
[[nodiscard]] base::Status EncodeQuote(const model::QuoteTick& quote,
                                       QuoteFrame& out) noexcept;

// Decodes one exact frame; failures preserve out. The receiver must set the
// returned zero receive_time_ns to its own context clock before dispatch.
[[nodiscard]] base::Status DecodeQuote(std::span<const std::byte> frame,
                                       model::QuoteTick& out) noexcept;

// Builds the transport envelope, borrowing frame until Publish returns.
// Call only after EncodeQuote succeeded for this quote and frame.
EventView QuoteEvent(const model::QuoteTick& quote,
                     const QuoteFrame& frame) noexcept;

// Validates both the envelope and frame; rejects unsupported event types and
// inconsistent source/instrument/sequence/time. Failure preserves out.
[[nodiscard]] base::Status DecodeQuoteEvent(const EventView& event,
                                            model::QuoteTick& out) noexcept;

}  // namespace yquant::md

#endif  // YQUANT_MD_QUOTE_CODEC_H_
