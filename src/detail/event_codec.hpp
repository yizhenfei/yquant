#pragma once

#include <yquant/md/shm_pubsub.hpp>

#include <cstdint>
#include <span>

namespace yquant::md::detail {

Result<void> encode_event(EventView event, std::span<std::uint64_t> words,
                          std::size_t max_payload_size);
Result<void> decode_event(std::span<const std::uint64_t> words,
                          std::size_t max_payload_size, EventBuffer& output);

} // namespace yquant::md::detail
