#ifndef YQUANT_BASE_INTERNAL_LOGGING_SHARED_H_
#define YQUANT_BASE_INTERNAL_LOGGING_SHARED_H_

#include <stdint.h>

#include <atomic>

#include "yquant/base/spsc_queue.h"

namespace yquant::base::internal {

inline constexpr uint64_t kSharedLogMagic = 0x59514c4f47303031ULL;

// One session per queue file; only logd initializes these pointer-free fields.
// The initialized file is published atomically. Release/acquire publication of
// producer_done orders the final queue write before logd's last drain; logd
// publishes consumer_done only after closing and syncing the output.
struct alignas(SpscQueue::required_alignment()) SharedLogState {
  uint64_t magic = kSharedLogMagic;
  uint64_t capacity = 0;
  std::atomic<uint64_t> producer_claimed{0};
  std::atomic<uint64_t> producer_done{0};
  std::atomic<uint64_t> consumer_done{0};
  std::atomic<uint64_t> queue_fault{0};
  std::atomic<uint64_t> backend_error{0};
  std::atomic<uint64_t> last_write_ns{0};
};

// The following queue begins immediately after this header.
static_assert(sizeof(SharedLogState) == SpscQueue::required_alignment());
static_assert(std::atomic<uint64_t>::is_always_lock_free);

}  // namespace yquant::base::internal

#endif  // YQUANT_BASE_INTERNAL_LOGGING_SHARED_H_
