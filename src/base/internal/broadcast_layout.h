#ifndef YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_
#define YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <cstddef>

#include "yquant/base/broadcast_queue.h"

namespace yquant::base::internal {
using AtomicWord = std::atomic<uint64_t>;
static_assert(AtomicWord::is_always_lock_free);
inline constexpr uint64_t kMaxBroadcastSequence = (uint64_t{1} << 63) - 1;
enum class BroadcastState : uint64_t { kReady = 1, kFinished = 2 };

// Immutable after initialization; validated from bytes before accessing
// atomics.
struct BroadcastLayout {
  uint64_t magic{};
  uint64_t version{};
  uint64_t abi{};
  uint64_t endian{};
  uint64_t bytes{};
  uint64_t capacity{};
  uint64_t max_record_size{};
  uint64_t word_count{};
  uint64_t slot_stride{};
  uint64_t slots_offset{};
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
AtomicWord& BroadcastSlot(std::byte* slots, size_t stride, size_t capacity,
                          uint64_t sequence, size_t word) noexcept;
}  // namespace yquant::base::internal
#endif  // YQUANT_BASE_INTERNAL_BROADCAST_LAYOUT_H_
