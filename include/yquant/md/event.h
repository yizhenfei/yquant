#ifndef YQUANT_MD_EVENT_H_
#define YQUANT_MD_EVENT_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstddef>
#include <span>

namespace yquant::md {

// Maximum payload bytes supported by event buffers and the broadcast codec;
// excludes the event header. Individual streams may configure a smaller limit.
inline constexpr size_t kMaxEventPayloadSize = 4096;

enum class EventScope : uint8_t {
  kBusiness,
  kStreamStatus,
  kInstrumentStatus,
};

struct EventHeader {
  uint64_t source_id = 0;
  uint64_t instrument_id = 0;
  uint64_t source_timestamp_ns = 0;
  uint64_t source_sequence = 0;
  uint32_t type = 0;
  EventScope scope = EventScope::kBusiness;
  bool has_source_sequence = false;
};

// The payload is borrowed for the duration of the consuming call.
struct EventView {
  EventHeader header;
  std::span<const std::byte> payload;
};

// Caller-owned storage. Only [0, payload_size) contains the current event.
struct EventBuffer {
  EventHeader header;
  std::array<std::byte, kMaxEventPayloadSize> payload{};
  size_t payload_size = 0;
  // Requires payload_size <= kMaxEventPayloadSize.
  EventView view() const noexcept {
    return {header, std::span(payload).first(payload_size)};
  }
};

}  // namespace yquant::md

#endif  // YQUANT_MD_EVENT_H_
