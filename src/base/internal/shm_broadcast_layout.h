#ifndef YQUANT_BASE_INTERNAL_SHM_BROADCAST_LAYOUT_H_
#define YQUANT_BASE_INTERNAL_SHM_BROADCAST_LAYOUT_H_

#include <stdint.h>

#include <atomic>

#include "yquant/base/broadcast_queue.h"

namespace yquant::base::internal {
// SHM envelope only. Queue layout/state lives in the following independently
// validated region. Immutable metadata precedes heartbeat access.
struct ShmBroadcastLayout {
  uint64_t magic{};
  uint64_t version{};
  uint64_t bytes{};
  uint64_t queue_offset{};
  uint64_t queue_bytes{};
  uint64_t heartbeat_size{};
  uint64_t heartbeat_alignment{};
  uint64_t heartbeat_offset{};
  BroadcastId generation;
  friend bool operator==(const ShmBroadcastLayout&,
                         const ShmBroadcastLayout&) = default;
};
struct alignas(BroadcastQueue::required_alignment()) ShmBroadcastControl {
  ShmBroadcastLayout layout;
  alignas(BroadcastQueue::required_alignment())
      std::atomic<uint64_t> heartbeat_ns{0};
};
}  // namespace yquant::base::internal
#endif  // YQUANT_BASE_INTERNAL_SHM_BROADCAST_LAYOUT_H_
