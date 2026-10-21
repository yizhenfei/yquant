#ifndef YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_
#define YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "yquant/base/broadcast_queue.h"

namespace yquant::base::internal {
using AtomicWord = std::atomic<std::uint64_t>;
static_assert(AtomicWord::is_always_lock_free);
inline constexpr std::uint64_t kMaxBroadcastSequence =
    (std::uint64_t{1} << 63) - 1;
enum class BroadcastState : std::uint64_t { kReady = 1, kFinished = 2 };

// Immutable after initialization; validated from bytes before accessing
// atomics.
struct BroadcastLayout {
  std::uint64_t magic{};
  std::uint64_t version{};
  std::uint64_t abi{};
  std::uint64_t endian{};
  std::uint64_t bytes{};
  std::uint64_t capacity{};
  std::uint64_t max_record_size{};
  std::uint64_t word_count{};
  std::uint64_t slot_stride{};
  std::uint64_t slots_offset{};
  BroadcastId generation;
  friend bool operator==(const BroadcastLayout&,
                         const BroadcastLayout&) = default;
};

struct alignas(BroadcastQueue::required_alignment()) BroadcastHeader {
  BroadcastLayout layout;
  alignas(BroadcastQueue::required_alignment()) AtomicWord state{0};
  alignas(BroadcastQueue::required_alignment()) AtomicWord published_sequence{
      0};
};

// Each slot contains stamp, record length, then a fixed number of payload
// words, all atomic. Caller must use a validated layout and sequence >= 1.
AtomicWord& BroadcastSlot(std::byte* slots, std::size_t stride,
                          std::size_t capacity, std::uint64_t sequence,
                          std::size_t word) noexcept;
}  // namespace yquant::base::internal
#endif  // YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_
