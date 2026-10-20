#include <yquant/md/shm_pubsub.hpp>

#include "detail/event_codec.hpp"
#include "detail/shm_segment.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <utility>

namespace yquant::md {
namespace {

Error error(ErrorCode code, const char* message) {
    return {code, 0, message};
}

void normalize(Subscription& subscription) {
    auto sort_unique = [](auto& values) {
        if (values) {
            std::sort(values->begin(), values->end());
            values->erase(std::unique(values->begin(), values->end()), values->end());
        }
    };
    sort_unique(subscription.instruments);
    sort_unique(subscription.event_types);
}

template<class T>
bool contains(const std::optional<std::vector<T>>& values, T value) {
    return !values || std::binary_search(values->begin(), values->end(), value);
}

bool matches(const Subscription& subscription, const EventHeader& header) {
    if (header.scope == EventScope::StreamStatus) {
        return true;
    }
    if (!contains(subscription.instruments, header.instrument_id)) {
        return false;
    }
    return header.scope == EventScope::InstrumentStatus ||
           contains(subscription.event_types, header.type);
}

Generation subscriber_instance() {
    std::random_device random;
    auto word = [&] {
        return (static_cast<std::uint64_t>(random()) << 32) |
               static_cast<std::uint64_t>(random());
    };
    Generation value;
    do {
        value = {word(), word()};
    } while (value == Generation{});
    return value;
}

Result<std::uint64_t> initial_position(const StartPosition& start,
                                      Generation generation,
                                      std::uint64_t published) {
    if (start.generation && *start.generation != generation) {
        return std::unexpected(error(ErrorCode::InvalidPosition,
                                     "Requested generation is not the current generation"));
    }
    if (start.mode == StartPosition::Mode::Live) {
        return published + 1;
    }
    if (start.mode != StartPosition::Mode::Sequence || start.sequence == 0 ||
        start.sequence > published + 1) {
        return std::unexpected(error(ErrorCode::InvalidPosition,
                                     "Start sequence must be in [1, published + 1]"));
    }
    return start.sequence;
}

} // namespace

struct ShmPublisher::Impl {
    std::unique_ptr<detail::Segment> segment;
    std::vector<std::uint64_t> encoded;
    std::uint64_t next_sequence{1};

    Impl(std::unique_ptr<detail::Segment> value, std::vector<std::uint64_t> words)
        : segment(std::move(value)), encoded(std::move(words)) {}
};

ShmPublisher::ShmPublisher(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ShmPublisher::ShmPublisher(ShmPublisher&&) noexcept = default;
ShmPublisher& ShmPublisher::operator=(ShmPublisher&&) noexcept = default;
ShmPublisher::~ShmPublisher() = default;

Result<ShmPublisher> ShmPublisher::create(const PublishConfig& config) {
    auto validated = detail::validate_config(config);
    if (!validated) {
        return std::unexpected(std::move(validated.error()));
    }
    if (config.max_payload_size > std::numeric_limits<std::uint32_t>::max() ||
        config.max_payload_size > std::numeric_limits<std::size_t>::max() -
                                      detail::wire_header_size - 7) {
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "Maximum payload size overflows the event layout"));
    }
    try {
        // Allocate private storage before Segment::create publishes the discovery entry.
        std::vector<std::uint64_t> encoded(
            (detail::wire_header_size + config.max_payload_size + 7) / 8);
        auto segment = detail::Segment::create(config);
        if (!segment) {
            return std::unexpected(std::move(segment.error()));
        }
        if ((*segment)->layout().word_count != encoded.size()) {
            return std::unexpected(error(ErrorCode::Incompatible,
                                         "Segment and event codec word counts disagree"));
        }
        return ShmPublisher(std::make_unique<Impl>(std::move(*segment), std::move(encoded)));
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(ErrorCode::SystemError,
                                     "Cannot allocate publisher private buffers"));
    } catch (const std::length_error&) {
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "Publisher private buffer exceeds the implementation limit"));
    }
}

Result<std::uint64_t> ShmPublisher::publish(EventView event) {
    if (!impl_) {
        return std::unexpected(error(ErrorCode::Closed, "Publisher is closed"));
    }
    if (!impl_->segment->owned_by_current_process()) {
        return std::unexpected(error(ErrorCode::Closed,
                                     "A forked process cannot publish through its parent's publisher"));
    }
    if (impl_->next_sequence > detail::max_sequence) {
        return std::unexpected(error(ErrorCode::SequenceExhausted,
                                     "Publisher sequence space is exhausted; create a new generation"));
    }
    auto encoded = detail::encode_event(event, impl_->encoded, max_payload_size());
    if (!encoded) {
        return std::unexpected(std::move(encoded.error()));
    }

    const auto sequence = impl_->next_sequence;
    auto& segment = *impl_->segment;
    segment.stamp(sequence).store(2 * sequence - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i != impl_->encoded.size(); ++i) {
        segment.word(sequence, i).store(impl_->encoded[i], std::memory_order_relaxed);
    }
    segment.stamp(sequence).store(2 * sequence, std::memory_order_release);
    segment.header().published_sequence.store(sequence, std::memory_order_release);
    impl_->next_sequence = sequence + 1;
    return sequence;
}

Result<void> ShmPublisher::tick() {
    if (!impl_) {
        return std::unexpected(error(ErrorCode::Closed, "Publisher is closed"));
    }
    if (!impl_->segment->owned_by_current_process()) {
        return std::unexpected(error(ErrorCode::Closed,
                                     "A forked process cannot update its parent's publisher heartbeat"));
    }
    impl_->segment->header().heartbeat_ns.store(detail::monotonic_ns(),
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

void ShmPublisher::close() noexcept { impl_.reset(); }

struct ShmSubscriber::Impl {
    std::unique_ptr<detail::Segment> segment;
    Subscription subscription;
    std::uint64_t next_sequence;
    std::vector<std::uint64_t> words;
    EventBuffer decoded;
    std::optional<GapToken> pending_gap;
    std::uint64_t gap_id{};
    std::optional<Error> sticky_error;
    std::optional<Generation> changed_generation;
    Generation instance;

    Impl(std::unique_ptr<detail::Segment> value, Subscription filter,
         std::uint64_t sequence)
        : segment(std::move(value)), subscription(std::move(filter)),
          next_sequence(sequence), words(segment->layout().word_count),
          decoded(segment->layout().max_payload_size), instance(subscriber_instance()) {}

    ReadResult result(ReadStatus status, std::size_t scanned = 0,
                      std::size_t attempts = 0) const {
        ReadResult value;
        value.status = status;
        value.generation = segment->layout().generation;
        value.sequence = next_sequence;
        value.scanned = scanned;
        value.attempts = attempts;
        return value;
    }

    ReadResult fail(Error failure, std::size_t scanned, std::size_t attempts,
                    bool sticky) {
        auto value = result(ReadStatus::Error, scanned, attempts);
        if (sticky) {
            sticky_error = std::move(failure);
            value.error = sticky_error;
        } else {
            value.error = std::move(failure);
        }
        return value;
    }

    ReadResult gap(std::uint64_t observed_sequence, std::size_t scanned,
                   std::size_t attempts) {
        ++gap_id;
        pending_gap = GapToken{segment->layout().generation, gap_id, next_sequence,
                               observed_sequence - segment->layout().capacity + 1,
                               instance};
        auto value = result(ReadStatus::Gap, scanned, attempts);
        value.gap = pending_gap;
        return value;
    }

    bool valid_stamp(std::uint64_t stamp) const noexcept {
        if (stamp == 0) {
            return true;
        }
        const auto sequence = (stamp >> 1) + (stamp & 1);
        const auto mask = segment->layout().capacity - 1;
        return sequence <= detail::max_sequence &&
               ((sequence - 1) & mask) == ((next_sequence - 1) & mask);
    }
};

ShmSubscriber::ShmSubscriber(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ShmSubscriber::ShmSubscriber(ShmSubscriber&&) noexcept = default;
ShmSubscriber& ShmSubscriber::operator=(ShmSubscriber&&) noexcept = default;
ShmSubscriber::~ShmSubscriber() = default;

Result<ShmSubscriber> ShmSubscriber::attach(const Endpoint& endpoint,
                                         Subscription subscription,
                                         StartPosition start) {
    try {
        normalize(subscription);
        auto segment = detail::Segment::attach(endpoint);
        if (!segment) {
            return std::unexpected(std::move(segment.error()));
        }
        const auto published = (*segment)->header().published_sequence.load(
            std::memory_order_acquire);
        if (published > detail::max_sequence) {
            return std::unexpected(error(ErrorCode::Incompatible,
                                         "Published sequence is outside the supported range"));
        }
        auto sequence = initial_position(start, (*segment)->layout().generation, published);
        if (!sequence) {
            return std::unexpected(std::move(sequence.error()));
        }
        return ShmSubscriber(std::make_unique<Impl>(std::move(*segment),
                                                  std::move(subscription), *sequence));
    } catch (const std::bad_alloc&) {
        return std::unexpected(error(ErrorCode::SystemError,
                                     "Cannot allocate subscriber private buffers"));
    } catch (const std::length_error&) {
        return std::unexpected(error(ErrorCode::Incompatible,
                                     "Subscriber private buffer exceeds the implementation limit"));
    } catch (const std::runtime_error&) {
        return std::unexpected(error(ErrorCode::SystemError,
                                     "Cannot generate subscriber instance identity"));
    }
}

ReadResult ShmSubscriber::try_next(EventBuffer& buffer, ReadBudget budget) {
    if (!impl_) {
        ReadResult value;
        value.status = ReadStatus::Closed;
        return value;
    }
    auto& impl = *impl_;
    if (impl.changed_generation) {
        return impl.result(ReadStatus::StreamChanged);
    }
    if (impl.sticky_error) {
        auto value = impl.result(ReadStatus::Error);
        value.error = impl.sticky_error;
        return value;
    }
    if (impl.pending_gap) {
        auto value = impl.result(ReadStatus::Gap);
        value.gap = impl.pending_gap;
        return value;
    }
    if (budget.max_scanned == 0 || budget.max_attempts == 0) {
        return impl.fail(error(ErrorCode::InvalidArgument,
                               "Read budgets must both be greater than zero"), 0, 0, false);
    }

    std::size_t scanned = 0;
    std::size_t attempts = 0;
    auto& segment = *impl.segment;
    for (;;) {
        if (scanned == budget.max_scanned) {
            return impl.result(ReadStatus::Yield, scanned, attempts);
        }
        if (attempts == budget.max_attempts) {
            return impl.result(ReadStatus::Retry, scanned, attempts);
        }

        // Observing Stopped before the tail synchronizes with the final commit.
        const auto state = segment.header().state.load(std::memory_order_acquire);
        const auto published = segment.header().published_sequence.load(std::memory_order_acquire);
        if ((state != static_cast<std::uint64_t>(detail::SegmentState::Ready) &&
             state != static_cast<std::uint64_t>(detail::SegmentState::Stopped)) ||
            published > detail::max_sequence) {
            return impl.fail(error(ErrorCode::Incompatible,
                                   "Shared control fields are outside the supported range"),
                             scanned, attempts, true);
        }
        if (impl.next_sequence > published) {
            return impl.result(state == static_cast<std::uint64_t>(detail::SegmentState::Stopped)
                                   ? ReadStatus::EndOfGeneration : ReadStatus::Empty,
                               scanned, attempts);
        }

        const auto sequence = impl.next_sequence;
        const auto first = segment.stamp(sequence).load(std::memory_order_acquire);
        ++attempts;
        if (!impl.valid_stamp(first)) {
            return impl.fail(error(ErrorCode::Incompatible, "Invalid shared slot stamp"),
                             scanned, attempts, true);
        }
        const auto first_sequence = (first >> 1) + (first & 1);
        if (first_sequence > sequence) {
            return impl.gap(first_sequence, scanned, attempts);
        }
        if (first != 2 * sequence) {
            continue;
        }

        for (std::size_t i = 0; i != impl.words.size(); ++i) {
            impl.words[i] = segment.word(sequence, i).load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const auto last = segment.stamp(sequence).load(std::memory_order_relaxed);
        if (!impl.valid_stamp(last)) {
            return impl.fail(error(ErrorCode::Incompatible, "Invalid shared slot stamp"),
                             scanned, attempts, true);
        }
        const auto last_sequence = (last >> 1) + (last & 1);
        if (last_sequence > sequence) {
            return impl.gap(last_sequence, scanned, attempts);
        }
        if (first != last) {
            continue;
        }

        auto decoded = detail::decode_event(impl.words, segment.layout().max_payload_size,
                                            impl.decoded);
        if (!decoded) {
            return impl.fail(std::move(decoded.error()), scanned, attempts, true);
        }
        ++scanned;
        if (!matches(impl.subscription, impl.decoded.header)) {
            ++impl.next_sequence;
            continue;
        }
        if (buffer.payload.capacity() < impl.decoded.payload.size()) {
            return impl.fail(error(ErrorCode::BufferTooSmall,
                                   "Event buffer capacity is smaller than the event payload"),
                             scanned, attempts, false);
        }
        buffer.payload.resize(impl.decoded.payload.size());
        std::copy(impl.decoded.payload.begin(), impl.decoded.payload.end(), buffer.payload.begin());
        buffer.header = impl.decoded.header;
        auto value = impl.result(ReadStatus::Event, scanned, attempts);
        ++impl.next_sequence;
        return value;
    }
}

Result<std::uint64_t> ShmSubscriber::set_subscription(Subscription subscription) {
    if (!impl_) {
        return std::unexpected(error(ErrorCode::Closed, "Subscriber is closed"));
    }
    normalize(subscription);
    impl_->subscription = std::move(subscription);
    return impl_->next_sequence;
}

Result<std::uint64_t> ShmSubscriber::resume(const GapToken& token, ResumePosition position) {
    if (!impl_) {
        return std::unexpected(error(ErrorCode::Closed, "Subscriber is closed"));
    }
    auto& impl = *impl_;
    if (impl.changed_generation || !impl.pending_gap || *impl.pending_gap != token ||
        token.generation != impl.segment->layout().generation) {
        return std::unexpected(error(ErrorCode::InvalidGap,
                                     "Gap token does not identify this subscriber's current gap"));
    }
    const auto published = impl.segment->header().published_sequence.load(std::memory_order_acquire);
    if (published > detail::max_sequence) {
        return std::unexpected(error(ErrorCode::Incompatible,
                                     "Published sequence is outside the supported range"));
    }
    std::uint64_t sequence{};
    switch (position.mode) {
    case ResumePosition::Mode::Candidate:
        sequence = token.resume_candidate;
        break;
    case ResumePosition::Mode::Live:
        sequence = published + 1;
        break;
    case ResumePosition::Mode::Sequence:
        sequence = position.sequence;
        break;
    default:
        return std::unexpected(error(ErrorCode::InvalidPosition, "Unknown resume position mode"));
    }
    if (sequence == 0 || sequence > published + 1) {
        return std::unexpected(error(ErrorCode::InvalidPosition,
                                     "Resume sequence must be in [1, published + 1]"));
    }
    impl.next_sequence = sequence;
    impl.pending_gap.reset();
    return sequence;
}

Result<RefreshResult> ShmSubscriber::refresh(std::chrono::nanoseconds stale_after) {
    if (!impl_) {
        return std::unexpected(error(ErrorCode::Closed, "Subscriber is closed"));
    }
    if (stale_after < std::chrono::nanoseconds::zero()) {
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "Heartbeat stale threshold must not be negative"));
    }
    auto& impl = *impl_;
    RefreshResult value;
    const auto state = impl.segment->header().state.load(std::memory_order_acquire);
    if (state == static_cast<std::uint64_t>(detail::SegmentState::Stopped)) {
        value.health = PublisherHealth::Stopped;
    } else if (state == static_cast<std::uint64_t>(detail::SegmentState::Ready)) {
        const auto heartbeat = impl.segment->header().heartbeat_ns.load(std::memory_order_acquire);
        const auto now = detail::monotonic_ns();
        value.health = now >= heartbeat && now - heartbeat >
                           static_cast<std::uint64_t>(stale_after.count())
                           ? PublisherHealth::Stale : PublisherHealth::Ready;
    }

    if (impl.changed_generation) {
        value.status = RefreshStatus::StreamChanged;
        value.discovered_generation = impl.changed_generation;
        return value;
    }

    auto manifest = detail::discover(impl.segment->endpoint());
    if (!manifest) {
        if (manifest.error().code == ErrorCode::NotFound) {
            value.status = RefreshStatus::Missing;
            return value;
        }
        return std::unexpected(std::move(manifest.error()));
    }
    if (manifest->generation != impl.segment->layout().generation) {
        impl.changed_generation = manifest->generation;
        impl.pending_gap.reset();
    }
    if (impl.changed_generation) {
        value.status = RefreshStatus::StreamChanged;
        value.discovered_generation = impl.changed_generation;
    } else {
        value.status = RefreshStatus::Current;
        value.discovered_generation = manifest->generation;
    }
    return value;
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

void ShmSubscriber::close() noexcept { impl_.reset(); }

} // namespace yquant::md
