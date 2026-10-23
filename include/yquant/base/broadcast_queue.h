#ifndef YQUANT_BASE_BROADCAST_QUEUE_H_
#define YQUANT_BASE_BROADCAST_QUEUE_H_

#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <optional>
#include <span>

#include "yquant/base/status.h"

namespace yquant::base {
namespace internal {
struct BroadcastHeader;
}

// Caller-provided identity of one initialization; never reuse it while an old
// endpoint or gap token can still be used.
struct BroadcastId {
  uint64_t high = 0;
  uint64_t low = 0;
  friend bool operator==(const BroadcastId&, const BroadcastId&) = default;
};

struct BroadcastConfig {
  size_t capacity = 1024;  // Nonzero power of two, measured in records.
  size_t max_record_size = 256;
};

struct BroadcastStartPosition {
  enum class Mode { kLive, kSequence } mode = Mode::kLive;
  uint64_t sequence = 0;
  std::optional<BroadcastId> generation;
  static BroadcastStartPosition Live() { return {}; }
  static BroadcastStartPosition At(uint64_t sequence) {
    return {Mode::kSequence, sequence, std::nullopt};
  }
  static BroadcastStartPosition At(BroadcastId id, uint64_t sequence) {
    return {Mode::kSequence, sequence, id};
  }
};

// Maximum slot-copy attempts, including attempts discarded after a conflict.
struct BroadcastReadBudget {
  size_t max_attempts = 128;
};

// Bound to one queue initialization, reader attach session and pending gap.
struct BroadcastGapToken {
  BroadcastId generation;
  uint64_t id = 0;
  uint64_t expected_sequence = 0;
  uint64_t resume_candidate = 0;
  BroadcastId reader_instance;
  friend bool operator==(const BroadcastGapToken&,
                         const BroadcastGapToken&) = default;
};

struct BroadcastResumePosition {
  enum class Mode { kCandidate, kLive, kSequence } mode = Mode::kCandidate;
  uint64_t sequence = 0;
  static BroadcastResumePosition Candidate() { return {}; }
  static BroadcastResumePosition Live() { return {Mode::kLive, 0}; }
  static BroadcastResumePosition At(uint64_t sequence) {
    return {Mode::kSequence, sequence};
  }
};

enum class BroadcastReadStatus {
  kRecord,
  kEmpty,
  kRetry,
  kGap,
  kEnd,
  kDetached,
  kError,
};

struct BroadcastReadResult {
  BroadcastReadStatus status = BroadcastReadStatus::kEmpty;
  BroadcastId generation;
  uint64_t sequence = 0;
  size_t attempts = 0;
  // Consistent private copy; valid until the next TryRead or Detach. Empty on
  // non-record results. Even a failed next read may overwrite old scratch data.
  std::span<const std::byte> record;
  std::optional<BroadcastGapToken> gap;
  Status error;
};

// Non-owning view over externally managed aligned memory, ordinary or shared.
// Exactly one writer and any number of readers may access initialized storage.
// Each endpoint requires serialized calls; initialization/reinitialization and
// freeing storage require exclusive access with all old endpoints detached.
// All operations are allocation-free and perform no system calls. Shared use
// additionally requires a platform with validated cross-process atomic support.
class BroadcastQueue final {
 public:
  BroadcastQueue() = default;
  BroadcastQueue(const BroadcastQueue&) = default;
  BroadcastQueue& operator=(const BroadcastQueue&) = default;

  struct SizeResult {
    Status status;
    size_t bytes = 0;
  };
  static constexpr size_t required_alignment() noexcept { return 128; }
  // Returns zero bytes on failure; checks arithmetic overflow without touching
  // storage. The returned size excludes any OS page rounding.
  [[nodiscard]] static SizeResult RequiredBytes(
      BroadcastConfig config) noexcept;
  // Requires exclusive access, aligned storage and nonzero instance ID. Failure
  // leaves memory unchanged. Initialization must be externally synchronized
  // before any other thread/process attaches.
  [[nodiscard]] static Status Initialize(std::span<std::byte> memory,
                                         BroadcastConfig config,
                                         BroadcastId instance_id) noexcept;
  // Validates immutable metadata and atomic ABI before accessing atomics.
  // Failure leaves a detached view detached; reattach fails without changing
  // an already attached view.
  [[nodiscard]] Status Attach(std::span<std::byte> memory) noexcept;
  void Detach() noexcept;
  bool attached() const noexcept { return header_ != nullptr; }
  size_t capacity() const noexcept { return capacity_; }
  size_t max_record_size() const noexcept { return max_record_size_; }
  BroadcastId generation() const noexcept { return generation_; }
  // True after Finish; detached views return false.
  bool finished() const noexcept;

 private:
  friend class BroadcastWriter;
  friend class BroadcastReader;
  std::span<std::byte> memory_;
  internal::BroadcastHeader* header_ = nullptr;
  std::byte* slots_ = nullptr;
  size_t capacity_ = 0;
  size_t max_record_size_ = 0;
  size_t word_count_ = 0;
  size_t slot_stride_ = 0;
  BroadcastId generation_;
};

// Non-owning, single-thread-owned write endpoint. Detach/destruction do not
// finish the stream. A replacement writer requires external ownership transfer;
// Attach resumes at the published tail. Finish permanently ends this instance.
class BroadcastWriter final {
 public:
  BroadcastWriter() = default;
  BroadcastWriter(const BroadcastWriter&) = delete;
  BroadcastWriter& operator=(const BroadcastWriter&) = delete;
  // Requires a detached endpoint and an unfinished queue. Failure preserves an
  // existing attachment, or leaves a previously detached endpoint detached.
  [[nodiscard]] Status Attach(std::span<std::byte> memory) noexcept;
  // Copies before returning; input must not overlap queue storage. Failure
  // commits nothing and leaves optional sequence unchanged. No reader blocks
  // it.
  [[nodiscard]] Status TryWrite(std::span<const std::byte> record,
                                uint64_t* sequence = nullptr) noexcept;
  // Idempotent while attached. Readers drain the final commit before kEnd.
  [[nodiscard]] Status Finish() noexcept;
  void Detach() noexcept;
  const BroadcastQueue& queue() const noexcept { return queue_; }

 private:
  BroadcastQueue queue_;
  uint64_t next_sequence_ = 1;
};

// Non-owning, single-thread-owned reader with an independent private cursor.
// Scratch is borrowed exclusively until Detach and must not overlap the queue
// or another reader's scratch. No shared reader state is written.
class BroadcastReader final {
 public:
  BroadcastReader() = default;
  BroadcastReader(const BroadcastReader&) = delete;
  BroadcastReader& operator=(const BroadcastReader&) = delete;
  // Scratch must fit max_record_size. Failure keeps a detached endpoint
  // detached; attempting reattach preserves the already attached endpoint.
  // At(sequence) may select overwritten data; TryRead then reports a gap.
  // A nonzero reader_identity must be unique per attach session across all
  // processes while its tokens exist. Zero generates a process-local identity;
  // shared-memory wrappers supply a fresh globally unique identity.
  [[nodiscard]] Status Attach(std::span<std::byte> memory,
                              std::span<std::byte> scratch,
                              BroadcastStartPosition start = {},
                              BroadcastId reader_identity = {}) noexcept;
  // Only kRecord advances the cursor. Zero budget is a nonsticky error. Gaps
  // and corrupt queue data remain sticky; no partial record is returned.
  [[nodiscard]] BroadcastReadResult TryRead(
      BroadcastReadBudget budget = {}) noexcept;
  // Requires this attach session's current token. Failure preserves gap,
  // cursor and output. Candidate may already have been overwritten again.
  [[nodiscard]] Status Resume(const BroadcastGapToken& token,
                              uint64_t& sequence,
                              BroadcastResumePosition position = {}) noexcept;
  void Detach() noexcept;
  uint64_t next_sequence() const noexcept {
    return queue_.attached() ? next_sequence_ : 0;
  }
  const BroadcastQueue& queue() const noexcept { return queue_; }

 private:
  // Builds a result at the current cursor, without exposing scratch contents.
  BroadcastReadResult Result(BroadcastReadStatus status,
                             size_t attempts = 0) const noexcept;
  // Records corruption, which must not be skipped by subsequent calls.
  BroadcastReadResult Fail(size_t attempts) noexcept;
  BroadcastQueue queue_;
  std::span<std::byte> scratch_;
  uint64_t next_sequence_ = 1;
  uint64_t gap_id_ = 0;
  BroadcastId session_;
  std::optional<BroadcastGapToken> pending_gap_;
  bool corrupt_ = false;
};

}  // namespace yquant::base
#endif  // YQUANT_BASE_BROADCAST_QUEUE_H_
