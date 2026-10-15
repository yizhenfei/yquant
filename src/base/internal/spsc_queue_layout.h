#ifndef YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_
#define YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace yquant::base::internal {

inline constexpr std::size_t kSpscAlignment = 128;
inline constexpr std::uint32_t kSpscLayoutVersion = 4;
inline constexpr std::uint64_t kSpscMagic = 0x5951535053435134ULL;
inline constexpr std::size_t kSpscFrameHeaderBytes = 4;
inline constexpr std::uint32_t kSpscWrapMarker =
    std::numeric_limits<std::uint32_t>::max();

// Only fixed-size, pointer-free state is placed in the queue storage.
struct alignas(kSpscAlignment) SpscMetadata {
  std::uint64_t magic = 0;
  std::uint32_t version = 0;
  std::uint64_t data_offset = 0;
  std::uint64_t capacity = 0;
};

struct alignas(kSpscAlignment) SpscCursor {
  std::atomic<std::uint64_t> position{0};
};

struct alignas(kSpscAlignment) SpscControlBlock {
  SpscMetadata metadata;
  SpscCursor write;
  SpscCursor read;
};

inline constexpr std::size_t kSpscDataOffset = 3 * kSpscAlignment;

static_assert(std::is_standard_layout_v<SpscControlBlock>);
static_assert(std::is_trivially_copyable_v<SpscMetadata>);
static_assert(sizeof(SpscMetadata) == kSpscAlignment);
static_assert(sizeof(SpscCursor) == kSpscAlignment);
static_assert(offsetof(SpscControlBlock, write) == kSpscAlignment);
static_assert(offsetof(SpscControlBlock, read) == 2 * kSpscAlignment);
static_assert(sizeof(SpscControlBlock) == kSpscDataOffset);

}  // namespace yquant::base::internal

#endif  // YQUANT_BASE_INTERNAL_SPSC_QUEUE_LAYOUT_H_
