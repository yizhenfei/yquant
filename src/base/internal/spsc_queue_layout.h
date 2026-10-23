#ifndef YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_
#define YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <limits>
#include <type_traits>

namespace yquant::base::internal {

inline constexpr size_t kSpscAlignment = 128;
inline constexpr uint32_t kSpscLayoutVersion = 4;
inline constexpr uint64_t kSpscMagic = 0x5951535053435134ULL;
inline constexpr size_t kSpscFrameHeaderBytes = 4;
inline constexpr uint32_t kSpscWrapMarker =
    std::numeric_limits<uint32_t>::max();

// Only fixed-size, pointer-free state is placed in the queue storage.
struct alignas(kSpscAlignment) SpscMetadata {
  uint64_t magic = 0;
  uint32_t version = 0;
  uint64_t data_offset = 0;
  uint64_t capacity = 0;
};

struct alignas(kSpscAlignment) SpscCursor {
  std::atomic<uint64_t> position{0};
};

struct alignas(kSpscAlignment) SpscControlBlock {
  SpscMetadata metadata;
  SpscCursor write;
  SpscCursor read;
};

inline constexpr size_t kSpscDataOffset = 3 * kSpscAlignment;

static_assert(std::is_standard_layout_v<SpscControlBlock>);
static_assert(std::is_trivially_copyable_v<SpscMetadata>);
static_assert(sizeof(SpscMetadata) == kSpscAlignment);
static_assert(sizeof(SpscCursor) == kSpscAlignment);
static_assert(offsetof(SpscControlBlock, write) == kSpscAlignment);
static_assert(offsetof(SpscControlBlock, read) == 2 * kSpscAlignment);
static_assert(sizeof(SpscControlBlock) == kSpscDataOffset);

}  // namespace yquant::base::internal

#endif  // YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_
