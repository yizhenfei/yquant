#include "yquant/md/shm_pubsub.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <new>
#include <utility>

#include "md/internal/event_codec.h"
#include "md/internal/shm_segment.h"

namespace yquant::md {
namespace {

constexpr std::size_t kMaxWordCount =
    (internal::kWireHeaderSize + kMaxEventPayloadSize + 7) / 8;

base::Status Error(base::ErrorCode code) { return {code, {}}; }

// Validate counts before forming iterators, then sort the owned filter in
// place.
base::Status Normalize(Subscription& subscription) {
  if (subscription.instrument_count > subscription.instruments.size() ||
      subscription.event_type_count > subscription.event_types.size()) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  std::sort(subscription.instruments.begin(),
            subscription.instruments.begin() + subscription.instrument_count);
  std::sort(subscription.event_types.begin(),
            subscription.event_types.begin() + subscription.event_type_count);
  return {};
}

bool Matches(const Subscription& subscription, const EventHeader& header) {
  if (header.scope == EventScope::kStreamStatus) return true;
  if (!subscription.all_instruments &&
      !std::binary_search(
          subscription.instruments.begin(),
          subscription.instruments.begin() + subscription.instrument_count,
          header.instrument_id)) {
    return false;
  }
  return header.scope == EventScope::kInstrumentStatus ||
         subscription.all_event_types ||
         std::binary_search(
             subscription.event_types.begin(),
             subscription.event_types.begin() + subscription.event_type_count,
             header.type);
}

// The sequence may already be overwritten; the read path reports that gap.
base::Status InitialPosition(const StartPosition& start, Generation generation,
                             std::uint64_t published, std::uint64_t& sequence) {
  if (start.generation && *start.generation != generation) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  if (start.mode == StartPosition::Mode::kLive) {
    sequence = published + 1;
    return {};
  }
  if (start.mode != StartPosition::Mode::kSequence || start.sequence == 0 ||
      start.sequence > published + 1) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  sequence = start.sequence;
  return {};
}

}  // namespace

base::Status ValidatePublishConfig(const PublishConfig& config) noexcept {
  auto validated = internal::ValidateConfig(config);
  return validated ? base::Status{} : validated.error();
}

struct ShmPublisher::Impl {
  std::unique_ptr<internal::Segment> segment;
  std::array<std::uint64_t, kMaxWordCount> encoded{};
  std::uint64_t next_sequence = 1;
};

ShmPublisher::ShmPublisher() noexcept = default;
ShmPublisher::ShmPublisher(ShmPublisher&&) noexcept = default;
ShmPublisher& ShmPublisher::operator=(ShmPublisher&&) noexcept = default;
ShmPublisher::~ShmPublisher() = default;

base::Status ShmPublisher::Create(const PublishConfig& config) {
  if (impl_) return Error(base::ErrorCode::kInvalidState);
  const auto validated = ValidatePublishConfig(config);
  if (!validated.ok()) return validated;
  // Allocate private state before making the new discovery entry visible.
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) return Error(base::ErrorCode::kResourceExhausted);
  auto segment = internal::Segment::Create(config);
  if (!segment) return segment.error();
  impl->segment = std::move(*segment);
  impl_ = std::move(impl);
  return {};
}

base::Status ShmPublisher::Publish(EventView event, std::uint64_t* sequence) {
  if (!impl_ || !impl_->segment->owned_by_current_process()) {
    return Error(base::ErrorCode::kInvalidState);
  }
  if (impl_->next_sequence > internal::kMaxSequence) {
    return Error(base::ErrorCode::kResourceExhausted);
  }
  auto& segment = *impl_->segment;
  auto words = std::span(impl_->encoded).first(segment.layout().word_count);
  auto encoded = internal::EncodeEvent(event, words, max_payload_size());
  if (!encoded) return encoded.error();

  const auto next = impl_->next_sequence;
  segment.stamp(next).store(2 * next - 1, std::memory_order_relaxed);
  // The odd stamp must precede every payload store. Atomic payload words avoid
  // data races even when a reader overlaps an overwrite and discards its copy.
  std::atomic_thread_fence(std::memory_order_release);
  for (std::size_t i = 0; i != words.size(); ++i) {
    segment.word(next, i).store(words[i], std::memory_order_relaxed);
  }
  segment.stamp(next).store(2 * next, std::memory_order_release);
  segment.header().published_sequence.store(next, std::memory_order_release);
  impl_->next_sequence = next + 1;
  if (sequence) *sequence = next;
  return {};
}

base::Status ShmPublisher::Tick() {
  if (!impl_ || !impl_->segment->owned_by_current_process()) {
    return Error(base::ErrorCode::kInvalidState);
  }
  impl_->segment->header().heartbeat_ns.store(internal::MonotonicNs(),
                                              std::memory_order_release);
  return {};
}

Generation ShmPublisher::generation() const noexcept {
  return impl_ ? impl_->segment->layout().generation : Generation{};
}
std::size_t ShmPublisher::capacity() const noexcept {
  return impl_ ? impl_->segment->layout().capacity : 0;
}
std::size_t ShmPublisher::max_payload_size() const noexcept {
  return impl_ ? impl_->segment->layout().max_payload_size : 0;
}
void ShmPublisher::Close() noexcept { impl_.reset(); }

struct ShmSubscriber::Impl {
  std::unique_ptr<internal::Segment> segment;
  Subscription subscription;
  std::uint64_t next_sequence = 1;
  std::array<std::uint64_t, kMaxWordCount> words{};
  EventBuffer decoded;
  std::optional<GapToken> pending_gap;
  std::uint64_t gap_id = 0;
  std::optional<base::Status> sticky_error;
  std::optional<Generation> changed_generation;
  Generation instance;

  ReadResult Result(ReadStatus status, std::size_t scanned = 0,
                    std::size_t attempts = 0) const {
    ReadResult value;
    value.status = status;
    value.generation = segment->layout().generation;
    value.sequence = next_sequence;
    value.scanned = scanned;
    value.attempts = attempts;
    return value;
  }

  // Sticky failures prohibit silently continuing after format corruption.
  ReadResult Fail(base::Status failure, std::size_t scanned,
                  std::size_t attempts, bool sticky) {
    auto value = Result(ReadStatus::kError, scanned, attempts);
    if (sticky) sticky_error = failure;
    value.error = failure;
    return value;
  }

  ReadResult Gap(std::uint64_t observed_sequence, std::size_t scanned,
                 std::size_t attempts) {
    ++gap_id;
    pending_gap =
        GapToken{segment->layout().generation, gap_id, next_sequence,
                 observed_sequence - segment->layout().capacity + 1, instance};
    auto value = Result(ReadStatus::kGap, scanned, attempts);
    value.gap = pending_gap;
    return value;
  }

  // Each stamp must identify the exact ring index being inspected.
  bool ValidStamp(std::uint64_t stamp) const noexcept {
    if (stamp == 0) return true;
    const auto sequence = (stamp >> 1) + (stamp & 1);
    const auto mask = segment->layout().capacity - 1;
    return sequence <= internal::kMaxSequence &&
           ((sequence - 1) & mask) == ((next_sequence - 1) & mask);
  }
};

ShmSubscriber::ShmSubscriber() noexcept = default;
ShmSubscriber::ShmSubscriber(ShmSubscriber&&) noexcept = default;
ShmSubscriber& ShmSubscriber::operator=(ShmSubscriber&&) noexcept = default;
ShmSubscriber::~ShmSubscriber() = default;

base::Status ShmSubscriber::Open(const Endpoint& endpoint,
                                 Subscription subscription,
                                 StartPosition start) {
  if (impl_) return Error(base::ErrorCode::kInvalidState);
  auto normalized = Normalize(subscription);
  if (!normalized.ok()) return normalized;
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) return Error(base::ErrorCode::kResourceExhausted);
  auto identity = internal::RandomGeneration();
  if (!identity) return identity.error();
  auto segment = internal::Segment::Attach(endpoint);
  if (!segment) return segment.error();
  const auto published =
      (*segment)->header().published_sequence.load(std::memory_order_acquire);
  if (published > internal::kMaxSequence) {
    return Error(base::ErrorCode::kCorrupt);
  }
  auto positioned = InitialPosition(start, (*segment)->layout().generation,
                                    published, impl->next_sequence);
  if (!positioned.ok()) return positioned;
  impl->subscription = subscription;
  impl->instance = *identity;
  impl->segment = std::move(*segment);
  impl_ = std::move(impl);
  return {};
}

ReadResult ShmSubscriber::TryNext(EventBuffer& buffer, ReadBudget budget) {
  if (!impl_) {
    ReadResult value;
    value.status = ReadStatus::kClosed;
    return value;
  }
  auto& impl = *impl_;
  if (impl.changed_generation) return impl.Result(ReadStatus::kStreamChanged);
  if (impl.sticky_error) {
    auto value = impl.Result(ReadStatus::kError);
    value.error = *impl.sticky_error;
    return value;
  }
  if (impl.pending_gap) {
    auto value = impl.Result(ReadStatus::kGap);
    value.gap = impl.pending_gap;
    return value;
  }
  if (budget.max_scanned == 0 || budget.max_attempts == 0) {
    return impl.Fail(Error(base::ErrorCode::kInvalidArgument), 0, 0, false);
  }

  std::size_t scanned = 0;
  std::size_t attempts = 0;
  auto& segment = *impl.segment;
  for (;;) {
    if (scanned == budget.max_scanned) {
      return impl.Result(ReadStatus::kYield, scanned, attempts);
    }
    if (attempts == budget.max_attempts) {
      return impl.Result(ReadStatus::kRetry, scanned, attempts);
    }
    // Observing Stopped before the tail synchronizes with the final commit.
    const auto state = segment.header().state.load(std::memory_order_acquire);
    const auto published =
        segment.header().published_sequence.load(std::memory_order_acquire);
    if ((state != static_cast<std::uint64_t>(internal::SegmentState::kReady) &&
         state !=
             static_cast<std::uint64_t>(internal::SegmentState::kStopped)) ||
        published > internal::kMaxSequence) {
      return impl.Fail(Error(base::ErrorCode::kCorrupt), scanned, attempts,
                       true);
    }
    if (impl.next_sequence > published) {
      return impl.Result(
          state == static_cast<std::uint64_t>(internal::SegmentState::kStopped)
              ? ReadStatus::kEndOfGeneration
              : ReadStatus::kEmpty,
          scanned, attempts);
    }

    const auto sequence = impl.next_sequence;
    const auto first = segment.stamp(sequence).load(std::memory_order_acquire);
    ++attempts;
    if (!impl.ValidStamp(first)) {
      return impl.Fail(Error(base::ErrorCode::kCorrupt), scanned, attempts,
                       true);
    }
    const auto first_sequence = (first >> 1) + (first & 1);
    if (first_sequence > sequence) {
      return impl.Gap(first_sequence, scanned, attempts);
    }
    if (first != 2 * sequence) continue;

    auto words = std::span(impl.words).first(segment.layout().word_count);
    for (std::size_t i = 0; i != words.size(); ++i) {
      words[i] = segment.word(sequence, i).load(std::memory_order_relaxed);
    }
    // The payload loads must precede stamp revalidation. The acquire fence also
    // synchronizes with an overwriter's release fence via atomic payload words.
    std::atomic_thread_fence(std::memory_order_acquire);
    const auto last = segment.stamp(sequence).load(std::memory_order_relaxed);
    if (!impl.ValidStamp(last)) {
      return impl.Fail(Error(base::ErrorCode::kCorrupt), scanned, attempts,
                       true);
    }
    const auto last_sequence = (last >> 1) + (last & 1);
    if (last_sequence > sequence)
      return impl.Gap(last_sequence, scanned, attempts);
    if (first != last) continue;

    auto decoded = internal::DecodeEvent(
        words, segment.layout().max_payload_size, impl.decoded);
    if (!decoded) return impl.Fail(decoded.error(), scanned, attempts, true);
    ++scanned;
    if (!Matches(impl.subscription, impl.decoded.header)) {
      ++impl.next_sequence;
      continue;
    }
    std::copy_n(impl.decoded.payload.begin(), impl.decoded.payload_size,
                buffer.payload.begin());
    buffer.payload_size = impl.decoded.payload_size;
    buffer.header = impl.decoded.header;
    auto value = impl.Result(ReadStatus::kEvent, scanned, attempts);
    ++impl.next_sequence;
    return value;
  }
}

base::Status ShmSubscriber::SetSubscription(Subscription subscription,
                                            std::uint64_t& effective_sequence) {
  if (!impl_) return Error(base::ErrorCode::kInvalidState);
  auto normalized = Normalize(subscription);
  if (!normalized.ok()) return normalized;
  impl_->subscription = subscription;
  effective_sequence = impl_->next_sequence;
  return {};
}

base::Status ShmSubscriber::Resume(const GapToken& token, std::uint64_t& output,
                                   ResumePosition position) {
  if (!impl_) return Error(base::ErrorCode::kInvalidState);
  auto& impl = *impl_;
  if (impl.changed_generation || !impl.pending_gap ||
      *impl.pending_gap != token ||
      token.generation != impl.segment->layout().generation) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  const auto published =
      impl.segment->header().published_sequence.load(std::memory_order_acquire);
  if (published > internal::kMaxSequence) {
    return Error(base::ErrorCode::kCorrupt);
  }
  std::uint64_t sequence = 0;
  switch (position.mode) {
    case ResumePosition::Mode::kCandidate:
      sequence = token.resume_candidate;
      break;
    case ResumePosition::Mode::kLive:
      sequence = published + 1;
      break;
    case ResumePosition::Mode::kSequence:
      sequence = position.sequence;
      break;
    default:
      return Error(base::ErrorCode::kInvalidArgument);
  }
  if (sequence == 0 || sequence > published + 1) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  impl.next_sequence = sequence;
  impl.pending_gap.reset();
  output = sequence;
  return {};
}

base::Status ShmSubscriber::Refresh(RefreshResult& output,
                                    std::chrono::nanoseconds stale_after) {
  if (!impl_) return Error(base::ErrorCode::kInvalidState);
  if (stale_after < std::chrono::nanoseconds::zero()) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  auto& impl = *impl_;
  RefreshResult value;
  const auto state =
      impl.segment->header().state.load(std::memory_order_acquire);
  if (state == static_cast<std::uint64_t>(internal::SegmentState::kStopped)) {
    value.health = PublisherHealth::kStopped;
  } else if (state ==
             static_cast<std::uint64_t>(internal::SegmentState::kReady)) {
    const auto heartbeat =
        impl.segment->header().heartbeat_ns.load(std::memory_order_acquire);
    const auto now = internal::MonotonicNs();
    value.health =
        now >= heartbeat && now - heartbeat >
                                static_cast<std::uint64_t>(stale_after.count())
            ? PublisherHealth::kStale
            : PublisherHealth::kReady;
  }
  if (impl.changed_generation) {
    value.status = RefreshStatus::kStreamChanged;
    value.discovered_generation = impl.changed_generation;
    output = value;
    return {};
  }
  auto manifest = internal::Discover(impl.segment->endpoint());
  if (!manifest) {
    if (manifest.error().system_error.value() == ENOENT) {
      value.status = RefreshStatus::kMissing;
      output = value;
      return {};
    }
    return manifest.error();
  }
  if (manifest->generation != impl.segment->layout().generation) {
    impl.changed_generation = manifest->generation;
    impl.pending_gap.reset();
  }
  if (impl.changed_generation) {
    value.status = RefreshStatus::kStreamChanged;
    value.discovered_generation = impl.changed_generation;
  } else {
    value.status = RefreshStatus::kCurrent;
    value.discovered_generation = manifest->generation;
  }
  output = value;
  return {};
}

Generation ShmSubscriber::generation() const noexcept {
  return impl_ ? impl_->segment->layout().generation : Generation{};
}
std::uint64_t ShmSubscriber::next_sequence() const noexcept {
  return impl_ ? impl_->next_sequence : 0;
}
std::size_t ShmSubscriber::max_payload_size() const noexcept {
  return impl_ ? impl_->segment->layout().max_payload_size : 0;
}
void ShmSubscriber::Close() noexcept { impl_.reset(); }

}  // namespace yquant::md
