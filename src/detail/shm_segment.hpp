#pragma once

#include <yquant/md/shm_pubsub.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace yquant::md::detail {

using AtomicWord = std::atomic<std::uint64_t>;
static_assert(AtomicWord::is_always_lock_free);
inline constexpr std::size_t cache_alignment = 128;
inline constexpr std::size_t wire_header_size = 64;
inline constexpr std::uint64_t layout_version = 1;
inline constexpr std::uint64_t event_schema_version = 1;
inline constexpr std::uint64_t segment_magic = 0x595155414e545348ULL;
inline constexpr std::uint64_t max_sequence = (std::uint64_t{1} << 63) - 1;

enum class SegmentState : std::uint64_t { Initializing, Ready, Stopped };

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

struct alignas(cache_alignment) SharedHeader {
    Bootstrap bootstrap;
    alignas(cache_alignment) AtomicWord state{0};
    alignas(cache_alignment) AtomicWord published_sequence{0};
    alignas(cache_alignment) AtomicWord heartbeat_ns{0};
};

struct Manifest {
    Generation generation;
    std::string shm_name;
};

std::uint64_t monotonic_ns() noexcept;
Result<void> validate_config(const PublishConfig& config);
Result<Manifest> discover(const Endpoint& endpoint);

class Segment {
public:
    static Result<std::unique_ptr<Segment>> create(const PublishConfig& config);
    static Result<std::unique_ptr<Segment>> attach(const Endpoint& endpoint);
    ~Segment();
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    SharedHeader& header() const noexcept;
    AtomicWord& stamp(std::uint64_t sequence) const noexcept;
    AtomicWord& word(std::uint64_t sequence, std::size_t index) const noexcept;
    const Bootstrap& layout() const noexcept;
    const Endpoint& endpoint() const noexcept;
    // A publisher inherited by fork must not publish or unlock the parent's lock.
    bool owned_by_current_process() const noexcept;
    void close() noexcept;

private:
    struct Impl;
    explicit Segment(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace yquant::md::detail
