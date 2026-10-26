#include "market/internal/event_codec.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>

namespace yquant::market::internal {
namespace {

template <typename T>
void Put(std::span<std::byte> bytes, size_t offset, T value) noexcept {
  for (size_t i = 0; i < sizeof(T); ++i) {
    bytes[offset + i] = static_cast<std::byte>(value & 0xffU);
    value >>= 8;
  }
}

template <typename T>
T Get(std::span<const std::byte> bytes, size_t offset) noexcept {
  T value{};
  for (size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(std::to_integer<unsigned int>(bytes[offset + i]))
             << (8 * i);
  }
  return value;
}

base::Status CodecError(base::ErrorCode code) { return {code, {}}; }

}  // namespace

Result<void> EncodeEvent(EventView event, std::span<uint64_t> words,
                         size_t max_payload_size) {
  if (max_payload_size > std::numeric_limits<uint32_t>::max() ||
      max_payload_size > std::numeric_limits<size_t>::max() - kWireHeaderSize ||
      words.size_bytes() < kWireHeaderSize + max_payload_size) {
    return Result<void>{CodecError(base::ErrorCode::kInvalidArgument)};
  }
  if (event.payload.size() > max_payload_size) {
    return Result<void>{CodecError(base::ErrorCode::kResourceExhausted)};
  }
  if (event.header.scope != EventScope::kBusiness &&
      event.header.scope != EventScope::kStreamStatus &&
      event.header.scope != EventScope::kInstrumentStatus) {
    return Result<void>{CodecError(base::ErrorCode::kCorrupt)};
  }

  // Both this buffer and EventView are process-private; shared storage is
  // accessed exclusively through atomic words by the ring implementation.
  std::fill(words.begin(), words.end(), uint64_t{0});
  auto bytes = std::as_writable_bytes(words);
  Put(bytes, 0, event.header.source_id);
  Put(bytes, 8, event.header.instrument_id);
  Put(bytes, 16, event.header.source_timestamp_ns);
  Put(bytes, 24,
      event.header.has_source_sequence ? event.header.source_sequence : 0);
  Put(bytes, 32, event.header.type);
  bytes[36] = static_cast<std::byte>(event.header.scope);
  bytes[37] = static_cast<std::byte>(event.header.has_source_sequence ? 1 : 0);
  Put(bytes, 40, static_cast<uint32_t>(event.payload.size()));
  Put(bytes, 44, static_cast<uint32_t>(kEventSchemaVersion));
  if (!event.payload.empty()) {
    std::memcpy(bytes.data() + kWireHeaderSize, event.payload.data(),
                event.payload.size());
  }
  return {};
}

Result<void> DecodeEvent(std::span<const uint64_t> words,
                         size_t max_payload_size, EventBuffer& output) {
  const auto bytes = std::as_bytes(words);
  if (bytes.size() < kWireHeaderSize) {
    return Result<void>{CodecError(base::ErrorCode::kCorrupt)};
  }
  const auto length = Get<uint32_t>(bytes, 40);
  const auto scope = std::to_integer<unsigned int>(bytes[36]);
  const auto flags = std::to_integer<unsigned int>(bytes[37]);
  if (scope > static_cast<unsigned int>(EventScope::kInstrumentStatus) ||
      flags > 1 || Get<uint16_t>(bytes, 38) != 0 ||
      Get<uint32_t>(bytes, 44) != kEventSchemaVersion ||
      length > max_payload_size || length > bytes.size() - kWireHeaderSize ||
      (!flags && Get<uint64_t>(bytes, 24) != 0) ||
      std::any_of(bytes.begin() + 48, bytes.begin() + kWireHeaderSize,
                  [](std::byte value) { return value != std::byte{0}; })) {
    return Result<void>{CodecError(base::ErrorCode::kCorrupt)};
  }
  if (output.payload.size() < length) {
    return Result<void>{CodecError(base::ErrorCode::kResourceExhausted)};
  }

  EventHeader header;
  header.source_id = Get<uint64_t>(bytes, 0);
  header.instrument_id = Get<uint64_t>(bytes, 8);
  header.source_timestamp_ns = Get<uint64_t>(bytes, 16);
  header.source_sequence = Get<uint64_t>(bytes, 24);
  header.type = Get<uint32_t>(bytes, 32);
  header.scope = static_cast<EventScope>(scope);
  header.has_source_sequence = flags != 0;
  output.payload_size = length;
  if (length != 0) {
    std::memcpy(output.payload.data(), bytes.data() + kWireHeaderSize, length);
  }
  output.header = header;
  return {};
}

}  // namespace yquant::market::internal
