#ifndef YQUANT_MD_SHM_PUBSUB_H_
#define YQUANT_MD_SHM_PUBSUB_H_

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "yquant/base/status.h"

namespace yquant::md {

inline constexpr std::size_t kMaxEventPayloadSize = 4096;
inline constexpr std::size_t kMaxSubscriptionEntries = 64;

struct Generation {
  std::uint64_t high = 0;
  std::uint64_t low = 0;
  friend bool operator==(const Generation&, const Generation&) = default;
};

// The runtime directory must be absolute, user-owned and not group/world
// writable.
struct Endpoint {
  std::filesystem::path runtime_directory;
  std::string stream;
};

struct PublishConfig {
  Endpoint endpoint;
  std::size_t capacity = 1024;
  std::size_t max_payload_size = 256;
};

// Validates endpoint spelling and the exact platform layout without opening or
// creating files. The same validation is used by Create, so callers can reject
// incompatible capacities before publishing any other configured outputs.
[[nodiscard]] base::Status ValidatePublishConfig(
    const PublishConfig& config) noexcept;

enum class EventScope : std::uint8_t {
  kBusiness,
  kStreamStatus,
  kInstrumentStatus,
};

struct EventHeader {
  std::uint64_t source_id = 0;
  std::uint64_t instrument_id = 0;
  std::uint64_t source_timestamp_ns = 0;
  std::uint64_t source_sequence = 0;
  std::uint32_t type = 0;
  EventScope scope = EventScope::kBusiness;
  bool has_source_sequence = false;
};

// The payload is borrowed for the duration of the consuming call.
struct EventView {
  EventHeader header;
  std::span<const std::byte> payload;
};

// Caller-owned storage. Only [0, payload_size) contains the current event.
struct EventBuffer {
  EventHeader header;
  std::array<std::byte, kMaxEventPayloadSize> payload{};
  std::size_t payload_size = 0;
  // Requires payload_size <= kMaxEventPayloadSize.
  EventView view() const noexcept {
    return {header, std::span(payload).first(payload_size)};
  }
};

// Entries past count are ignored. An unrestricted filter matches everything;
// a restricted filter with zero entries matches nothing. Stream status always
// passes; instrument status ignores the event type filter.
struct Subscription {
  std::array<std::uint64_t, kMaxSubscriptionEntries> instruments{};
  std::size_t instrument_count = 0;
  bool all_instruments = true;
  std::array<std::uint32_t, kMaxSubscriptionEntries> event_types{};
  std::size_t event_type_count = 0;
  bool all_event_types = true;
};

struct StartPosition {
  enum class Mode { kLive, kSequence } mode = Mode::kLive;
  std::uint64_t sequence = 0;
  std::optional<Generation> generation;
  static StartPosition Live() { return {}; }
  static StartPosition At(std::uint64_t sequence) {
    return {Mode::kSequence, sequence, std::nullopt};
  }
  static StartPosition At(Generation generation, std::uint64_t sequence) {
    return {Mode::kSequence, sequence, generation};
  }
};

struct ReadBudget {
  std::size_t max_scanned = 64;
  std::size_t max_attempts = 128;
};

// Opaque capability bound to one subscriber and its currently pending gap.
struct GapToken {
  Generation generation;
  std::uint64_t id = 0;
  std::uint64_t expected_sequence = 0;
  std::uint64_t resume_candidate = 0;
  Generation subscriber_instance;
  friend bool operator==(const GapToken&, const GapToken&) = default;
};

enum class ReadStatus {
  kEvent,
  kEmpty,
  kYield,
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
  std::uint64_t sequence = 0;
  std::size_t scanned = 0;
  std::size_t attempts = 0;
  std::optional<GapToken> gap;
  base::Status error;
};

struct ResumePosition {
  enum class Mode { kCandidate, kLive, kSequence } mode = Mode::kCandidate;
  std::uint64_t sequence = 0;
  static ResumePosition Candidate() { return {}; }
  static ResumePosition Live() { return {Mode::kLive, 0}; }
  static ResumePosition At(std::uint64_t sequence) {
    return {Mode::kSequence, sequence};
  }
};

enum class PublisherHealth { kReady, kStale, kStopped, kUnknown };
enum class RefreshStatus { kCurrent, kStreamChanged, kMissing };
struct RefreshResult {
  RefreshStatus status = RefreshStatus::kCurrent;
  PublisherHealth health = PublisherHealth::kUnknown;
  std::optional<Generation> discovered_generation;
};

// Single-thread-owned publisher. Constructors do not allocate. Create acquires
// a stable-inode flock and publishes a new generation. Subscribers never block
// publication; overwritten data is reported as a gap by each subscriber.
class ShmPublisher {
 public:
  ShmPublisher() noexcept;
  ShmPublisher(ShmPublisher&&) noexcept;
  ShmPublisher& operator=(ShmPublisher&&) noexcept;
  ~ShmPublisher();
  ShmPublisher(const ShmPublisher&) = delete;
  ShmPublisher& operator=(const ShmPublisher&) = delete;

  // Requires a closed object, power-of-two capacity and payload limit <= 4096.
  // Failure leaves the object closed; the stable lock file is never removed.
  [[nodiscard]] base::Status Create(const PublishConfig& config);
  // Copies event before returning. On failure, nothing is published and the
  // optional sequence output is unchanged. No allocation occurs on this path.
  [[nodiscard]] base::Status Publish(EventView event,
                                     std::uint64_t* sequence = nullptr);
  // Updates liveness while idle. Inherited publishers cannot publish or tick.
  [[nodiscard]] base::Status Tick();
  Generation generation() const noexcept;
  std::size_t capacity() const noexcept;
  std::size_t max_payload_size() const noexcept;
  // Idempotent; stops the generation, retires discovery and releases the lock.
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Single-thread-owned subscriber with an independent private cursor. It never
// writes shared reader state and must be reopened explicitly after stream
// change.
class ShmSubscriber {
 public:
  ShmSubscriber() noexcept;
  ShmSubscriber(ShmSubscriber&&) noexcept;
  ShmSubscriber& operator=(ShmSubscriber&&) noexcept;
  ~ShmSubscriber();
  ShmSubscriber(const ShmSubscriber&) = delete;
  ShmSubscriber& operator=(const ShmSubscriber&) = delete;

  // Requires a closed object. Failure leaves it closed. At(sequence) can name
  // overwritten data; the first read then reports a gap rather than skipping
  // it.
  [[nodiscard]] base::Status Open(const Endpoint& endpoint,
                                  Subscription subscription = {},
                                  StartPosition start = StartPosition::Live());
  // Copies one matching event into buffer only on kEvent. Other results leave
  // buffer unchanged. Budgets must be nonzero. Gap and corrupt-data errors are
  // sticky; only Resume clears a gap. This path does not allocate or perform
  // I/O.
  [[nodiscard]] ReadResult TryNext(EventBuffer& buffer, ReadBudget budget = {});
  // Takes effect at the current cursor, returned on success. Pending gaps
  // remain. Invalid counts fail without changing the filter or output.
  [[nodiscard]] base::Status SetSubscription(Subscription subscription,
                                             std::uint64_t& effective_sequence);
  // Requires this subscriber's current token. Candidate can already be stale.
  // On failure the gap, cursor and output remain unchanged.
  [[nodiscard]] base::Status Resume(const GapToken& token,
                                    std::uint64_t& sequence,
                                    ResumePosition position = {});
  // Cold-path discovery and heartbeat check. A discovered replacement makes
  // stream change sticky. Failure leaves output unchanged. No automatic attach.
  [[nodiscard]] base::Status Refresh(
      RefreshResult& result,
      std::chrono::nanoseconds stale_after = std::chrono::seconds(3));
  Generation generation() const noexcept;
  std::uint64_t next_sequence() const noexcept;
  std::size_t max_payload_size() const noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_SHM_PUBSUB_H_
