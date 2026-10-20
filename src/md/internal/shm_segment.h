#ifndef YQUANT_MD_INTERNAL_SHM_SEGMENT_H_
#define YQUANT_MD_INTERNAL_SHM_SEGMENT_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "md/internal/result.h"
#include "yquant/md/shm_pubsub.h"

namespace yquant::md::internal {

using AtomicWord = std::atomic<std::uint64_t>;
static_assert(AtomicWord::is_always_lock_free);
inline constexpr std::size_t kCacheAlignment = 128;
inline constexpr std::size_t kWireHeaderSize = 64;
inline constexpr std::uint64_t kLayoutVersion = 1;
inline constexpr std::uint64_t kEventSchemaVersion = 1;
inline constexpr std::uint64_t kSegmentMagic = 0x595155414e545348ULL;
inline constexpr std::uint64_t kMaxSequence = (std::uint64_t{1} << 63) - 1;

enum class SegmentState : std::uint64_t { kInitializing, kReady, kStopped };

// Fixed-width bootstrap, read and validated before accessing any atomic object.
struct Bootstrap {
  std::uint64_t magic{};
  std::uint64_t version{};
  std::uint64_t schema{};
  std::uint64_t abi{};
  std::uint64_t endian{};
  std::uint64_t atomic_size{};
  std::uint64_t atomic_alignment{};
  std::uint64_t header_size{};
  std::uint64_t segment_size{};
  std::uint64_t capacity{};
  std::uint64_t max_payload_size{};
  std::uint64_t word_count{};
  std::uint64_t slot_stride{};
  std::uint64_t slots_offset{};
  Generation generation;
};

// Published once Ready is released. Payload and control words are only accessed
// atomically; immutable bootstrap bytes are validated before slot arithmetic.
struct alignas(kCacheAlignment) SharedHeader {
  Bootstrap bootstrap;
  alignas(kCacheAlignment) AtomicWord state{0};
  alignas(kCacheAlignment) AtomicWord published_sequence{0};
  alignas(kCacheAlignment) AtomicWord heartbeat_ns{0};
};

// Discovery entry atomically replaced by rename; never reused across
// generations.
struct Manifest {
  Generation generation;
  std::string shm_name;
};

// Uses the operating system entropy source. Failure returns no identity.
Result<Generation> RandomGeneration();
// Returns zero if the monotonic clock is unavailable.
std::uint64_t MonotonicNs() noexcept;
// Checks endpoint and allocation bounds without publishing resources.
Result<void> ValidateConfig(const PublishConfig& config);
// Reads current discovery through a private directory fd; ENOENT means absent.
Result<Manifest> Discover(const Endpoint& endpoint);

// Owns one mapping. A publisher additionally owns the stable lock and discovery
// entries until destruction. All methods require external thread serialization.
class Segment {
 public:
  // Acquires the unique-publisher lock before retiring old generations.
  // Failure cleans up owned mappings/discovery and returns no segment.
  static Result<std::unique_ptr<Segment>> Create(const PublishConfig& config);
  // Validates the bootstrap, atomic ABI and discovery identity before exposing
  // any slot. Never modifies discovery; a raced replacement can be retried.
  static Result<std::unique_ptr<Segment>> Attach(const Endpoint& endpoint);
  ~Segment();
  Segment(const Segment&) = delete;
  Segment& operator=(const Segment&) = delete;

  SharedHeader& header() const noexcept;
  // Requires sequence in [1, kMaxSequence] and an open segment.
  AtomicWord& stamp(std::uint64_t sequence) const noexcept;
  // Requires index < layout().word_count and a valid stamp sequence.
  AtomicWord& word(std::uint64_t sequence, std::size_t index) const noexcept;
  const Bootstrap& layout() const noexcept;
  const Endpoint& endpoint() const noexcept;
  // A publisher inherited by fork must not publish or unlock the parent's lock.
  bool owned_by_current_process() const noexcept;
  // Idempotent; inherited fork copies only release their local descriptors.
  void Close() noexcept;

 private:
  struct Impl;
  explicit Segment(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yquant::md::internal

#endif  // YQUANT_MD_INTERNAL_SHM_SEGMENT_H_
