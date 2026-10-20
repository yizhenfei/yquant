#include "detail/event_codec.hpp"
#include "detail/shm_segment.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace yquant::md::detail {
namespace {

template<class T>
void put(std::span<std::byte> bytes, std::size_t offset, T value) noexcept {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        bytes[offset + i] = static_cast<std::byte>(value & 0xffU);
        value >>= 8;
    }
}

template<class T>
T get(std::span<const std::byte> bytes, std::size_t offset) noexcept {
    T value{};
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(std::to_integer<unsigned int>(bytes[offset + i])) << (8 * i);
    }
    return value;
}

auto codec_error(ErrorCode code, const char* message) {
    return std::unexpected(Error{code, 0, message});
}

} // namespace

Result<void> encode_event(EventView event, std::span<std::uint64_t> words,
                          std::size_t max_payload_size) {
    if (max_payload_size > std::numeric_limits<std::uint32_t>::max() ||
        max_payload_size > std::numeric_limits<std::size_t>::max() - wire_header_size ||
        words.size_bytes() < wire_header_size + max_payload_size) {
        return codec_error(ErrorCode::InvalidArgument, "invalid event encoding buffer");
    }
    if (event.payload.size() > max_payload_size) {
        return codec_error(ErrorCode::EventTooLarge, "event exceeds configured payload size");
    }
    if (event.header.scope != EventScope::Business &&
        event.header.scope != EventScope::StreamStatus &&
        event.header.scope != EventScope::InstrumentStatus) {
        return codec_error(ErrorCode::InvalidEvent, "invalid event scope");
    }

    // Both this buffer and EventView are process-private; shared storage is
    // accessed exclusively through atomic words by the ring implementation.
    std::fill(words.begin(), words.end(), std::uint64_t{0});
    auto bytes = std::as_writable_bytes(words);
    put(bytes, 0, event.header.source_id);
    put(bytes, 8, event.header.instrument_id);
    put(bytes, 16, event.header.source_timestamp_ns);
    put(bytes, 24, event.header.has_source_sequence ? event.header.source_sequence : 0);
    put(bytes, 32, event.header.type);
    bytes[36] = static_cast<std::byte>(event.header.scope);
    bytes[37] = static_cast<std::byte>(event.header.has_source_sequence ? 1 : 0);
    put(bytes, 40, static_cast<std::uint32_t>(event.payload.size()));
    put(bytes, 44, static_cast<std::uint32_t>(event_schema_version));
    if (!event.payload.empty()) {
        std::memcpy(bytes.data() + wire_header_size, event.payload.data(), event.payload.size());
    }
    return {};
}

Result<void> decode_event(std::span<const std::uint64_t> words,
                          std::size_t max_payload_size, EventBuffer& output) {
    const auto bytes = std::as_bytes(words);
    if (bytes.size() < wire_header_size) {
        return codec_error(ErrorCode::InvalidEvent, "truncated event header");
    }
    const auto length = get<std::uint32_t>(bytes, 40);
    const auto scope = std::to_integer<unsigned int>(bytes[36]);
    const auto flags = std::to_integer<unsigned int>(bytes[37]);
    if (scope > static_cast<unsigned int>(EventScope::InstrumentStatus) || flags > 1 ||
        get<std::uint16_t>(bytes, 38) != 0 ||
        get<std::uint32_t>(bytes, 44) != event_schema_version ||
        length > max_payload_size || length > bytes.size() - wire_header_size ||
        (!flags && get<std::uint64_t>(bytes, 24) != 0) ||
        std::any_of(bytes.begin() + 48, bytes.begin() + wire_header_size,
                    [](std::byte value) { return value != std::byte{0}; })) {
        return codec_error(ErrorCode::InvalidEvent, "invalid event header or payload length");
    }
    if (output.payload.capacity() < length) {
        return codec_error(ErrorCode::BufferTooSmall, "event output buffer needs more capacity");
    }

    EventHeader header;
    header.source_id = get<std::uint64_t>(bytes, 0);
    header.instrument_id = get<std::uint64_t>(bytes, 8);
    header.source_timestamp_ns = get<std::uint64_t>(bytes, 16);
    header.source_sequence = get<std::uint64_t>(bytes, 24);
    header.type = get<std::uint32_t>(bytes, 32);
    header.scope = static_cast<EventScope>(scope);
    header.has_source_sequence = flags != 0;
    output.payload.resize(length);
    if (length != 0) {
        std::memcpy(output.payload.data(), bytes.data() + wire_header_size, length);
    }
    output.header = header;
    return {};
}

} // namespace yquant::md::detail
