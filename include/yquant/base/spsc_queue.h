#ifndef YQUANT_BASE_SPSC_QUEUE_H_
#define YQUANT_BASE_SPSC_QUEUE_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace yquant::base {

namespace internal {
struct SpscControlBlock;
}

enum class SpscStatus {
  kSuccess,
  kInvalidArgument,
  kInsufficientStorage,
  kMisalignedStorage,
  kUnsupportedPlatform,
  kIncompatibleLayout,
  kInvalidState,
  kFull,
  kEmpty,
  kTooLarge,
  kBusy,
  kCorrupt,
  kInvalidSize,
  kBufferTooSmall,
};

// A non-owning endpoint for a queue in externally managed, aligned storage.
// Exactly one producer and one consumer may access the queue storage. Each
// endpoint and its outstanding borrow must be used serially by its owner.
class SpscQueue final {
 public:
  enum class Role { kUnbound, kProducer, kConsumer };

  struct SizeResult {
    SpscStatus status = SpscStatus::kInvalidState;
    std::size_t bytes = 0;
  };

  class WriteReservation final {
   public:
    WriteReservation() noexcept = default;
    ~WriteReservation() noexcept;

    WriteReservation(const WriteReservation&) = delete;
    WriteReservation& operator=(const WriteReservation&) = delete;
    WriteReservation(WriteReservation&& other) noexcept;
    WriteReservation& operator=(WriteReservation&& other) noexcept;

    [[nodiscard]] bool active() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] std::span<std::byte> payload() const noexcept;
    [[nodiscard]] SpscStatus Commit(std::size_t actual_size) noexcept;
    void Cancel() noexcept;

   private:
    friend class SpscQueue;
    WriteReservation(SpscQueue* owner, std::uint64_t frame_pos,
                     std::size_t reserved_size) noexcept;

    SpscQueue* owner_ = nullptr;
    std::uint64_t frame_pos_ = 0;
    std::size_t reserved_size_ = 0;
  };

  class ReadLease final {
   public:
    ReadLease() noexcept = default;
    ~ReadLease() noexcept;

    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
    ReadLease(ReadLease&& other) noexcept;
    ReadLease& operator=(ReadLease&& other) noexcept;

    [[nodiscard]] bool active() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] std::span<const std::byte> payload() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] SpscStatus Release() noexcept;
    void Cancel() noexcept;

   private:
    friend class SpscQueue;
    ReadLease(SpscQueue* owner, std::uint64_t frame_pos,
              std::size_t payload_size) noexcept;

    SpscQueue* owner_ = nullptr;
    std::uint64_t frame_pos_ = 0;
    std::size_t payload_size_ = 0;
  };

  struct ReserveResult;
  struct AcquireResult;

  struct ReadResult {
    SpscStatus status = SpscStatus::kInvalidState;
    std::size_t payload_size = 0;
  };

  SpscQueue() noexcept = default;
  // An outstanding borrow must finish before its endpoint is destroyed.
  ~SpscQueue() noexcept;

  SpscQueue(const SpscQueue&) = delete;
  SpscQueue& operator=(const SpscQueue&) = delete;
  SpscQueue(SpscQueue&&) = delete;
  SpscQueue& operator=(SpscQueue&&) = delete;

  [[nodiscard]] static constexpr std::size_t required_alignment() noexcept {
    return 128;
  }
  [[nodiscard]] static SizeResult RequiredBytes(
      std::uint64_t capacity) noexcept;
  // Requires exclusive access. Constructs queue state but does not bind a
  // view.
  [[nodiscard]] static SpscStatus Initialize(std::span<std::byte> memory,
                                             std::uint64_t capacity) noexcept;
  // Initialization must already be visible through external synchronization.
  [[nodiscard]] SpscStatus Attach(std::span<std::byte> memory,
                                  Role role) noexcept;
  [[nodiscard]] SpscStatus Detach() noexcept;

  [[nodiscard]] bool attached() const noexcept { return control_ != nullptr; }
  [[nodiscard]] Role role() const noexcept { return role_; }
  [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t max_payload_size() const noexcept;

  // A coarse total-space check; it does not reserve space or publish padding.
  [[nodiscard]] SpscStatus CheckWrite(std::size_t payload_size) const noexcept;
  [[nodiscard]] ReserveResult TryReserve(std::size_t max_payload_size) noexcept;
  [[nodiscard]] AcquireResult TryAcquire() noexcept;

  // Copy buffers must not overlap the queue storage. These operations can
  // publish/reclaim tail padding even when no data frame is written/read.
  [[nodiscard]] SpscStatus TryWrite(
      std::span<const std::byte> payload) noexcept;
  [[nodiscard]] ReadResult TryRead(std::span<std::byte> output) noexcept;

 private:
  internal::SpscControlBlock* control_ = nullptr;
  std::byte* data_ = nullptr;
  std::uint64_t capacity_ = 0;
  Role role_ = Role::kUnbound;
  bool borrow_active_ = false;
};

struct SpscQueue::ReserveResult {
  SpscStatus status = SpscStatus::kInvalidState;
  WriteReservation reservation;
};

struct SpscQueue::AcquireResult {
  SpscStatus status = SpscStatus::kInvalidState;
  ReadLease lease;
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_SPSC_QUEUE_H_
