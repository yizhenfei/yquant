#ifndef YQUANT_MD_SHM_BROADCAST_H_
#define YQUANT_MD_SHM_BROADCAST_H_

#include <stddef.h>
#include <stdint.h>

#include <chrono>
#include <memory>
#include <optional>

#include "yquant/base/shm_broadcast.h"
#include "yquant/base/status.h"
#include "yquant/md/event.h"
#include "yquant/md/pipeline.h"

namespace yquant::md {

using Generation = base::BroadcastId;
using Endpoint = base::ShmEndpoint;
using StartPosition = base::BroadcastStartPosition;
using ReadBudget = base::BroadcastReadBudget;
using GapToken = base::BroadcastGapToken;
using ResumePosition = base::BroadcastResumePosition;
using PublisherHealth = base::ShmWriterHealth;
using RefreshStatus = base::ShmRefreshStatus;
using RefreshResult = base::ShmRefreshResult;

struct PublishConfig {
  Endpoint endpoint;
  size_t capacity = 1024;
  size_t max_payload_size = 256;
};

// Validates endpoint spelling and the exact platform layout without opening or
// creating files. The same validation is used by Create, so callers can reject
// incompatible capacities before publishing any other configured outputs.
[[nodiscard]] base::Status ValidatePublishConfig(
    const PublishConfig& config) noexcept;

enum class ReadStatus {
  kEvent,
  kEmpty,
  kRetry,
  kGap,
  kStreamChanged,
  kEndOfGeneration,
  kClosed,
  kError,
};

struct ReadResult {
  ReadStatus status = ReadStatus::kEmpty;
  Generation generation;
  uint64_t sequence = 0;
  size_t attempts = 0;
  std::optional<GapToken> gap;
  base::Status error;
};

// Single-thread-owned market-event adapter over base::ShmBroadcastWriter.
// Owns reusable encoding storage; queue synchronization and SHM lifecycle
// are delegated to the byte broadcast layer. Constructors do not allocate.
class ShmPublisher final : public Publisher {
 public:
  ShmPublisher() noexcept;
  ShmPublisher(ShmPublisher&&) noexcept;
  ShmPublisher& operator=(ShmPublisher&&) noexcept;
  ~ShmPublisher() override;
  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;

  // Quote pipeline setup also requires room for a complete quote frame.
  [[nodiscard]] base::Status Init(const PublishConfig& config) noexcept;
  [[nodiscard]] base::Status Publish(
      const model::QuoteTick& quote) noexcept override;
  // Refreshes the heartbeat at most once per 250 ms.
  [[nodiscard]] base::Status Service(uint64_t now) noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

  // Requires a closed object, power-of-two capacity and payload limit <= 4096.
  // Failure leaves the object closed; the stable lock file is never removed.
  [[nodiscard]] base::Status Create(const PublishConfig& config);
  // Copies event before returning. On failure, nothing is published and the
  // optional sequence output is unchanged. No allocation occurs on this path.
  [[nodiscard]] base::Status Publish(EventView event,
                                     uint64_t* sequence = nullptr);
  // Updates liveness while idle. Inherited publishers cannot publish or tick.
  [[nodiscard]] base::Status Tick();
  Generation generation() const noexcept;
  size_t capacity() const noexcept;
  size_t max_payload_size() const noexcept;
  // Idempotent; stops the generation, retires discovery and releases the lock.
  void Close() noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  uint64_t next_tick_ = 0;
  char error_detail_[256] = {};
};

// Single-thread-owned market-event adapter over base::ShmBroadcastReader.
// Decodes consistent byte records without filtering; invalid event frames stop
// delivery. Owns decoding scratch and preserves caller output on failure.
class ShmReader {
 public:
  ShmReader() noexcept;
  ShmReader(ShmReader&&) noexcept;
  ShmReader& operator=(ShmReader&&) noexcept;
  ~ShmReader();
  ShmReader(const ShmReader&) = delete;
  ShmReader& operator=(const ShmReader&) = delete;

  // Requires a closed object. Failure leaves it closed. At(sequence) can name
  // overwritten data; the first read then reports a gap rather than skipping
  // it.
  [[nodiscard]] base::Status Open(const Endpoint& endpoint,
                                  StartPosition start = StartPosition::Live());
  // Copies the next event into buffer only on kEvent. Other results leave
  // buffer unchanged. max_attempts must be nonzero. Gap and corrupt-data errors
  // are sticky; only Resume clears a gap. No allocation or I/O on this path.
  [[nodiscard]] ReadResult TryNext(EventBuffer& buffer, ReadBudget budget = {});
  // Requires this reader's current token. Candidate can already be stale.
  // On failure the gap, cursor and output remain unchanged.
  [[nodiscard]] base::Status Resume(const GapToken& token, uint64_t& sequence,
                                    ResumePosition position = {});
  // Cold-path discovery and heartbeat check. A discovered replacement makes
  // stream change sticky. Failure leaves output unchanged. No automatic attach.
  [[nodiscard]] base::Status Refresh(
      RefreshResult& result,
      std::chrono::nanoseconds stale_after = std::chrono::seconds(3));
  Generation generation() const noexcept;
  uint64_t next_sequence() const noexcept;
  size_t max_payload_size() const noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_SHM_BROADCAST_H_
