#include <yquant/md/shm_pubsub.hpp>
#include "detail/event_codec.hpp"
#include "detail/shm_segment.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace yquant::md;
using namespace std::chrono_literals;

namespace {

#define CHECK(condition) do { if (!(condition)) throw std::runtime_error( \
    std::string(__func__) + ":" + std::to_string(__LINE__) + ": " #condition); } while (false)

template<class T> T take(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(*result);
}

void okay(Result<void> result) {
    if (!result) throw std::runtime_error(result.error().message);
}

struct Runtime {
    Endpoint endpoint;
    Runtime() {
        std::array<char, 64> pattern{};
        std::strcpy(pattern.data(), "/tmp/yquant-md-test-XXXXXX");
        const auto directory = ::mkdtemp(pattern.data());
        if (!directory) throw std::runtime_error("mkdtemp failed");
        endpoint = {directory, "ticks"};
    }
    ~Runtime() {
        // Runtime is declared before its publishers, so all locks are closed first.
        const auto stale = detail::discover(endpoint);
        if (stale) ::shm_unlink(stale->shm_name.c_str());
        std::error_code ignored;
        std::filesystem::remove_all(endpoint.runtime_directory, ignored);
    }
};

class Child {
public:
    explicit Child(std::function<void()> function) {
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("fork failed");
        if (pid_ == 0) {
            try {
                function();
                ::_exit(0);
            } catch (const std::exception& error) {
                std::cerr << "child: " << error.what() << std::endl;
                ::_exit(1);
            }
        }
    }
    ~Child() {
        if (pid_ > 0) {
            ::kill(pid_, SIGKILL);
            int status;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    void kill() { CHECK(::kill(pid_, SIGKILL) == 0); }
    int wait(std::chrono::milliseconds timeout = 10s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            int status;
            const auto result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                return status;
            }
            if (result < 0 && errno != EINTR) throw std::runtime_error("waitpid failed");
            std::this_thread::sleep_for(1ms);
        }
        throw std::runtime_error("child timed out");
    }
    void success() {
        const auto status = wait();
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
private:
    pid_t pid_{-1};
};

class Pipe {
public:
    Pipe() { CHECK(::pipe(fds_) == 0); }
    ~Pipe() { ::close(fds_[0]); ::close(fds_[1]); }
    void signal() { const char byte = 1; CHECK(::write(fds_[1], &byte, 1) == 1); }
    void wait() {
        pollfd descriptor{fds_[0], POLLIN, 0};
        CHECK(::poll(&descriptor, 1, 5000) == 1);
        char byte;
        CHECK(::read(fds_[0], &byte, 1) == 1);
    }
private:
    int fds_[2]{};
};

std::vector<std::byte> payload(std::uint64_t value, std::size_t size = 32) {
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        const auto word = value * 0x9e3779b97f4a7c15ULL + (i / 8) * 0x517cc1b727220a95ULL;
        bytes[i] = std::byte((word >> (8 * (i % 8))) & 0xff);
    }
    return bytes;
}

EventHeader header(std::uint64_t sequence, std::uint64_t instrument = 42,
                   std::uint32_t type = 7, EventScope scope = EventScope::Business) {
    EventHeader result;
    result.source_id = 13;
    result.instrument_id = instrument;
    result.source_timestamp_ns = sequence * 1000;
    result.source_sequence = sequence;
    result.type = type;
    result.scope = scope;
    result.has_source_sequence = true;
    return result;
}

void publish(ShmPublisher& publisher, std::uint64_t value, std::uint64_t instrument = 42,
             std::uint32_t type = 7, EventScope scope = EventScope::Business, std::size_t size = 32) {
    const auto bytes = payload(value, size);
    CHECK(take(publisher.publish({header(value, instrument, type, scope), bytes})) == value);
}

void expect_event(ShmSubscriber& subscriber, EventBuffer& buffer, std::uint64_t sequence) {
    const auto result = subscriber.try_next(buffer);
    CHECK(result.status == ReadStatus::Event);
    CHECK(result.sequence == sequence);
    CHECK(result.generation == subscriber.generation());
    CHECK(buffer.header.source_id == 13);
    CHECK(buffer.header.source_sequence == sequence);
    CHECK(buffer.header.source_timestamp_ns == sequence * 1000);
    CHECK(buffer.header.has_source_sequence);
    CHECK(buffer.payload == payload(sequence, buffer.payload.size()));
}

void multiple_readers_and_live_start() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 16, 64}));
    publish(publisher, 1, 42, 7, EventScope::Business, 13);
    publish(publisher, 2, 42, 7, EventScope::Business, 0);
    auto a = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    auto b = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    auto live = take(ShmSubscriber::attach(runtime.endpoint));
    CHECK(live.next_sequence() == 3);
    EventBuffer buffer(64);
    CHECK(live.try_next(buffer).status == ReadStatus::Empty);
    expect_event(a, buffer, 1);
    expect_event(a, buffer, 2);
    expect_event(b, buffer, 1);
    publish(publisher, 3);
    expect_event(live, buffer, 3);
    expect_event(b, buffer, 2);
    expect_event(b, buffer, 3);
    expect_event(a, buffer, 3);
    CHECK(a.try_next(buffer).status == ReadStatus::Empty);
}

void filtering_and_scan_budget() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 16, 64}));
    publish(publisher, 1, 99, 7);
    publish(publisher, 2, 42, 8);
    publish(publisher, 3, 99, 99, EventScope::StreamStatus);
    publish(publisher, 4, 99, 99, EventScope::InstrumentStatus);
    publish(publisher, 5, 42, 99, EventScope::InstrumentStatus);
    publish(publisher, 6, 42, 7);
    Subscription filter{std::vector<std::uint64_t>{42}, std::vector<std::uint32_t>{7}};
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, filter, StartPosition::at(1)));
    EventBuffer buffer(64);
    auto result = subscriber.try_next(buffer, {1, 8});
    CHECK(result.status == ReadStatus::Yield && result.scanned == 1);
    CHECK(subscriber.next_sequence() == 2);
    result = subscriber.try_next(buffer, {1, 8});
    CHECK(result.status == ReadStatus::Yield && result.scanned == 1);
    expect_event(subscriber, buffer, 3);
    expect_event(subscriber, buffer, 5);
    expect_event(subscriber, buffer, 6);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::Empty);

    // An empty instrument set matches no instruments; stream status remains visible.
    Subscription none{std::vector<std::uint64_t>{}, std::vector<std::uint32_t>{}};
    auto empty = take(ShmSubscriber::attach(runtime.endpoint, none, StartPosition::at(1)));
    EventBuffer no_capacity;
    auto forced = empty.try_next(no_capacity);
    CHECK(forced.status == ReadStatus::Error && forced.error->code == ErrorCode::BufferTooSmall);
    CHECK(empty.next_sequence() == 3); // filtered events require no caller buffer
    no_capacity.payload.reserve(64);
    expect_event(empty, no_capacity, 3);
    CHECK(empty.try_next(no_capacity).status == ReadStatus::Empty);
}

void subscription_boundary() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    publish(publisher, 1, 2);
    publish(publisher, 2, 1);
    publish(publisher, 3, 2);
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint,
        {std::vector<std::uint64_t>{1}, std::nullopt}, StartPosition::at(1)));
    EventBuffer buffer(64);
    CHECK(subscriber.try_next(buffer, {1, 8}).status == ReadStatus::Yield);
    CHECK(take(subscriber.set_subscription({std::vector<std::uint64_t>{2}, std::nullopt})) == 2);
    expect_event(subscriber, buffer, 3);
    CHECK(subscriber.next_sequence() == 4);
}

void publish_errors_and_small_buffer() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 32}));
    auto bytes = payload(1, 33);
    auto oversized = publisher.publish({header(1), bytes});
    CHECK(!oversized && oversized.error().code == ErrorCode::EventTooLarge);
    auto invalid = header(1);
    invalid.scope = static_cast<EventScope>(255);
    auto invalid_result = publisher.publish({invalid, {}});
    CHECK(!invalid_result && invalid_result.error().code == ErrorCode::InvalidEvent);
    publish(publisher, 1);
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    EventBuffer small;
    auto read = subscriber.try_next(small);
    CHECK(read.status == ReadStatus::Error && read.error->code == ErrorCode::BufferTooSmall);
    CHECK(subscriber.next_sequence() == 1);
    small.payload.reserve(32);
    expect_event(subscriber, small, 1);
    auto invalid_budget = subscriber.try_next(small, {0, 1});
    CHECK(invalid_budget.status == ReadStatus::Error && invalid_budget.error->code == ErrorCode::InvalidArgument);
    CHECK(subscriber.try_next(small).status == ReadStatus::Empty);
    subscriber.close();
    CHECK(subscriber.try_next(small).status == ReadStatus::Closed);
}

void overrun_and_explicit_recovery() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 4, 64}));
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) publish(publisher, sequence);
    EventBuffer buffer(64);
    auto result = subscriber.try_next(buffer);
    CHECK(result.status == ReadStatus::Gap && result.gap.has_value());
    const auto gap = *result.gap;
    CHECK(gap.expected_sequence == 1 && gap.resume_candidate == 2);
    CHECK(subscriber.next_sequence() == 1);
    CHECK(subscriber.try_next(buffer).gap == gap);
    CHECK(take(subscriber.set_subscription({})) == 1);
    CHECK(subscriber.try_next(buffer).gap == gap);
    auto forged = gap;
    ++forged.id;
    auto invalid = subscriber.resume(forged);
    CHECK(!invalid && invalid.error().code == ErrorCode::InvalidGap);
    auto zero = subscriber.resume(gap, ResumePosition::at(0));
    CHECK(!zero && zero.error().code == ErrorCode::InvalidPosition);
    auto other = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    CHECK(other.try_next(buffer).status == ReadStatus::Gap);
    auto foreign = other.resume(gap);
    CHECK(!foreign && foreign.error().code == ErrorCode::InvalidGap);
    // The candidate is a conservative observation, and may already be overwritten.
    CHECK(take(subscriber.resume(gap)) == 2);
    auto candidate_gap = subscriber.try_next(buffer);
    CHECK(candidate_gap.status == ReadStatus::Gap);
    CHECK(candidate_gap.gap->expected_sequence == 2);
    CHECK(take(subscriber.resume(*candidate_gap.gap, ResumePosition::at(5))) == 5);
    expect_event(subscriber, buffer, 5);
    CHECK(!subscriber.resume(gap));
    for (std::uint64_t sequence = 9; sequence <= 13; ++sequence) publish(publisher, sequence);
    auto second = subscriber.try_next(buffer);
    CHECK(second.status == ReadStatus::Gap && second.gap->id != gap.id);
    CHECK(second.gap->expected_sequence == 6 && second.gap->resume_candidate == 7);
    CHECK(!subscriber.resume(gap));
    CHECK(take(subscriber.resume(*second.gap, ResumePosition::live())) == 14);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::Empty);
    publish(publisher, 14);
    expect_event(subscriber, buffer, 14);
}

void stopped_generation_drains() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    publish(publisher, 1);
    publish(publisher, 2);
    publisher.close();
    EventBuffer buffer(64);
    expect_event(subscriber, buffer, 1);
    expect_event(subscriber, buffer, 2);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::EndOfGeneration);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::EndOfGeneration);
    auto published = publisher.publish({header(3), {}});
    CHECK(!published && published.error().code == ErrorCode::Closed);
    CHECK(!publisher.tick());
}

void refresh_and_restart_boundaries() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 2, 64}));
    const auto generation = publisher.generation();
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) publish(publisher, sequence);
    EventBuffer buffer(64);
    const auto gap = *subscriber.try_next(buffer).gap;
    std::this_thread::sleep_for(2ms);
    auto refresh = take(subscriber.refresh(1ns));
    CHECK(refresh.status == RefreshStatus::Current && refresh.health == PublisherHealth::Stale);
    okay(publisher.tick());
    CHECK(take(subscriber.refresh(3s)).health == PublisherHealth::Ready);
    publisher.close();
    CHECK(take(subscriber.refresh()).status == RefreshStatus::Missing);
    auto replacement = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    CHECK(replacement.generation() != generation);
    refresh = take(subscriber.refresh());
    CHECK(refresh.status == RefreshStatus::StreamChanged);
    CHECK(refresh.discovered_generation == replacement.generation());
    CHECK(subscriber.try_next(buffer).status == ReadStatus::StreamChanged);
    CHECK(!subscriber.resume(gap));
    auto reconnected = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    CHECK(!reconnected.resume(gap));
    publish(replacement, 1);
    expect_event(reconnected, buffer, 1);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::StreamChanged);
    CHECK(!ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(generation, 1)));
    replacement.close();
    CHECK(take(subscriber.refresh()).status == RefreshStatus::StreamChanged);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::StreamChanged);
}

void lock_survives_inherited_close() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    auto duplicate = ShmPublisher::create({runtime.endpoint, 8, 64});
    CHECK(!duplicate && duplicate.error().code == ErrorCode::AlreadyPublished);
    Child child([&] {
        auto foreign_publish = publisher.publish({header(1), {}});
        CHECK(!foreign_publish);
        publisher.close(); // Must close, never LOCK_UN, the inherited open file description.
        auto attempted = ShmPublisher::create({runtime.endpoint, 8, 64});
        CHECK(!attempted && attempted.error().code == ErrorCode::AlreadyPublished);
    });
    child.success();
    publish(publisher, 1);
    duplicate = ShmPublisher::create({runtime.endpoint, 8, 64});
    CHECK(!duplicate && duplicate.error().code == ErrorCode::AlreadyPublished);
    publisher.close();
    auto replacement = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    publish(replacement, 1);
}

void abrupt_exit_releases_lock() {
    Runtime runtime;
    Pipe ready;
    Child child([&] {
        auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
        publish(publisher, 1);
        ready.signal();
        // This process is killed by its parent; keep a bounded fallback.
        std::this_thread::sleep_for(8s);
        throw std::runtime_error("expected SIGKILL");
    });
    ready.wait();
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    const auto old = subscriber.generation();
    child.kill();
    const auto status = child.wait();
    CHECK(WIFSIGNALED(status));
    auto replacement = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    CHECK(replacement.generation() != old);
    CHECK(take(subscriber.refresh()).status == RefreshStatus::StreamChanged);
}

void pending_initialization_recovery() {
    Runtime runtime;
    Pipe ready;
    Child child([&] {
        auto publisher = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
        publish(publisher, 1);
        ready.signal();
        std::this_thread::sleep_for(8s);
        throw std::runtime_error("expected initialization crash");
    });
    ready.wait();
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    const auto previous = take(detail::discover(runtime.endpoint));
    struct Cleanup {
        std::string name;
        ~Cleanup() { ::shm_unlink(name.c_str()); }
    } cleanup{previous.shm_name};
    // Reproduce the persisted initialization record before stable discovery.
    std::filesystem::rename(runtime.endpoint.runtime_directory / "ticks.manifest",
                            runtime.endpoint.runtime_directory / "ticks.pending");
    child.kill();
    const auto status = child.wait();
    CHECK(WIFSIGNALED(status));
    auto replacement = take(ShmPublisher::create({runtime.endpoint, 8, 64}));
    CHECK(!std::filesystem::exists(runtime.endpoint.runtime_directory / "ticks.pending"));
    const auto stale_fd = ::shm_open(previous.shm_name.c_str(), O_RDWR, 0);
    const auto stale_errno = errno;
    if (stale_fd >= 0) ::close(stale_fd);
    CHECK(stale_fd < 0 && stale_errno == ENOENT);
    CHECK(take(subscriber.refresh()).status == RefreshStatus::StreamChanged);
    auto reconnected = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    CHECK(reconnected.generation() == replacement.generation());
    EventBuffer buffer(64);
    publish(replacement, 1);
    expect_event(reconnected, buffer, 1);
}

void atomic_aliases_at_distinct_addresses() {
    Runtime runtime;
    auto owner = take(detail::Segment::create({runtime.endpoint, 8, 64}));
    auto alias = take(detail::Segment::attach(runtime.endpoint));
    CHECK(&owner->header() != &alias->header());
    CHECK(&owner->word(1, 0) != &alias->word(1, 0));
    owner->word(1, 0).store(0x123456789abcdef0ULL, std::memory_order_release);
    CHECK(alias->word(1, 0).load(std::memory_order_acquire) == 0x123456789abcdef0ULL);
    Child child([&] {
        auto independent = take(detail::Segment::attach(runtime.endpoint));
        CHECK(&independent->header() != &owner->header());
        CHECK(&independent->header() != &alias->header());
        CHECK(independent->word(1, 0).load(std::memory_order_acquire) == 0x123456789abcdef0ULL);
        // Sequential platform probe of the same atomic through another process
        // and virtual address; this is outside the publication protocol.
        independent->word(1, 0).store(0xfedcba9876543210ULL, std::memory_order_release);
    });
    child.success();
    CHECK(owner->word(1, 0).load(std::memory_order_acquire) == 0xfedcba9876543210ULL);
    CHECK(alias->word(1, 0).load(std::memory_order_acquire) == 0xfedcba9876543210ULL);
}

void stalled_slot_and_precommit_overwrite() {
    Runtime runtime;
    auto segment = take(detail::Segment::create({runtime.endpoint, 4, 32}));
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    EventBuffer buffer(32);
    CHECK(subscriber.try_next(buffer).status == ReadStatus::Empty);
    segment->header().published_sequence.store(1, std::memory_order_release);
    segment->stamp(1).store(1, std::memory_order_release);
    auto stalled = subscriber.try_next(buffer, {8, 3});
    CHECK(stalled.status == ReadStatus::Retry && stalled.attempts == 3);
    CHECK(subscriber.next_sequence() == 1);
    segment->header().published_sequence.store(4, std::memory_order_release);
    segment->stamp(5).store(9, std::memory_order_release); // sequence 5 not committed; tail remains 4
    auto overwritten = subscriber.try_next(buffer);
    CHECK(overwritten.status == ReadStatus::Gap);
    CHECK(overwritten.gap->expected_sequence == 1 && overwritten.gap->resume_candidate == 2);
}

void reject_untrusted_layout() {
    Runtime runtime;
    auto segment = take(detail::Segment::create({runtime.endpoint, 8, 64}));
    auto& bootstrap = segment->header().bootstrap;
    const auto original = bootstrap;
    const auto rejected = [&] {
        auto subscriber = ShmSubscriber::attach(runtime.endpoint);
        CHECK(!subscriber && subscriber.error().code == ErrorCode::Incompatible);
    };
    bootstrap.abi ^= 1;
    rejected();
    bootstrap = original;
    bootstrap.slot_stride = ~std::uint64_t{0};
    rejected();
    bootstrap = original;
    bootstrap.slots_offset = 1;
    rejected();
    bootstrap = original;
    bootstrap.capacity = 3;
    rejected();
    bootstrap = original;
    bootstrap.segment_size += 4096;
    rejected();
    bootstrap = original;
    const auto manifest = take(detail::discover(runtime.endpoint));
    // Retain the original mapping, but replace the named object with a truncated
    // object. Darwin does not support resizing an already-sized shm object.
    CHECK(::shm_unlink(manifest.shm_name.c_str()) == 0);
    const int fd = ::shm_open(manifest.shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    auto zero_sized = ShmSubscriber::attach(runtime.endpoint);
    const auto sized = ::ftruncate(fd, 8);
    ::close(fd);
    CHECK(!zero_sized && zero_sized.error().code == ErrorCode::Incompatible);
    CHECK(sized == 0);
    auto truncated = ShmSubscriber::attach(runtime.endpoint);
    CHECK(!truncated && truncated.error().code == ErrorCode::Incompatible);
}

void concurrent_cross_process_integrity() {
    Runtime runtime;
    auto publisher = take(ShmPublisher::create({runtime.endpoint, 16, 512}));
    Pipe ready;
    Child child([&] {
        publisher.close(); // All reads below use an independent mapping.
        auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
        EventBuffer buffer(512);
        ready.signal();
        std::uint64_t previous = 0;
        std::size_t received = 0;
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = subscriber.try_next(buffer, {16, 32});
            if (result.status == ReadStatus::Event) {
                CHECK(result.sequence > previous);
                CHECK(buffer.header.source_sequence == result.sequence);
                CHECK(buffer.header.instrument_id == (result.sequence & 7));
                CHECK(buffer.header.source_timestamp_ns == result.sequence * 1000);
                CHECK(buffer.payload == payload(result.sequence, 512));
                previous = result.sequence;
                ++received;
            } else if (result.status == ReadStatus::Gap) {
                CHECK(subscriber.resume(*result.gap).has_value());
            } else if (result.status == ReadStatus::EndOfGeneration) {
                CHECK(received > 0);
                return;
            } else {
                CHECK(result.status == ReadStatus::Empty || result.status == ReadStatus::Retry ||
                      result.status == ReadStatus::Yield);
            }
        }
        throw std::runtime_error("stress reader timed out");
    });
    ready.wait();
    for (std::uint64_t sequence = 1; sequence <= 50'000; ++sequence)
        publish(publisher, sequence, sequence & 7, 7, EventScope::Business, 512);
    publisher.close();
    child.success();
}

void corrupt_event_stops_delivery() {
    Runtime runtime;
    auto segment = take(detail::Segment::create({runtime.endpoint, 8, 16}));
    const auto bytes = payload(1, 8);
    std::vector<std::uint64_t> encoded(segment->layout().word_count);
    okay(detail::encode_event({header(1), bytes}, encoded, 16));
    const auto valid = encoded;
    // A committed message must never turn an untrusted length into a read bound.
    auto wire = std::as_writable_bytes(std::span(encoded));
    std::fill(wire.begin() + 40, wire.begin() + 44, std::byte{0xff});
    for (std::size_t i = 0; i < encoded.size(); ++i)
        segment->word(1, i).store(encoded[i], std::memory_order_relaxed);
    segment->stamp(1).store(2, std::memory_order_release);
    segment->header().published_sequence.store(1, std::memory_order_release);
    auto subscriber = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    EventBuffer output(16);
    const auto malformed = subscriber.try_next(output);
    CHECK(malformed.status == ReadStatus::Error && malformed.error->code == ErrorCode::InvalidEvent);
    CHECK(subscriber.next_sequence() == 1);
    for (std::size_t i = 0; i < valid.size(); ++i)
        segment->word(1, i).store(valid[i], std::memory_order_relaxed);
    // A diagnosed format error remains visible even if shared storage changes.
    CHECK(subscriber.try_next(output).error->code == ErrorCode::InvalidEvent);
    auto fresh = take(ShmSubscriber::attach(runtime.endpoint, {}, StartPosition::at(1)));
    expect_event(fresh, output, 1);
}

void invalid_configuration() {
    Runtime runtime;
    CHECK(!ShmPublisher::create({runtime.endpoint, 0, 64}));
    CHECK(!ShmPublisher::create({runtime.endpoint, 3, 64}));
    CHECK(!ShmPublisher::create({runtime.endpoint, 8, std::numeric_limits<std::size_t>::max()}));
    CHECK(!ShmPublisher::create({runtime.endpoint, 8, std::numeric_limits<std::uint32_t>::max()}));
    CHECK(!ShmPublisher::create({{runtime.endpoint.runtime_directory, "../escape"}, 8, 64}));
    auto missing = ShmSubscriber::attach(runtime.endpoint);
    CHECK(!missing && missing.error().code == ErrorCode::NotFound);
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    const std::array tests{
        std::pair{"multiple readers and live start", multiple_readers_and_live_start},
        std::pair{"local filtering and bounded scanning", filtering_and_scan_budget},
        std::pair{"subscription effective boundary", subscription_boundary},
        std::pair{"publication and caller buffer errors", publish_errors_and_small_buffer},
        std::pair{"overrun and explicit gap recovery", overrun_and_explicit_recovery},
        std::pair{"normal stop drains generation", stopped_generation_drains},
        std::pair{"refresh and restart boundaries", refresh_and_restart_boundaries},
        std::pair{"publisher lock and fork inheritance", lock_survives_inherited_close},
        std::pair{"abrupt exit releases publisher lock", abrupt_exit_releases_lock},
        std::pair{"pending initialization crash recovery", pending_initialization_recovery},
        std::pair{"atomics across distinct virtual mappings", atomic_aliases_at_distinct_addresses},
        std::pair{"stalled slot and precommit overwrite", stalled_slot_and_precommit_overwrite},
        std::pair{"untrusted shared-memory layout", reject_untrusted_layout},
        std::pair{"cross-process overwrite integrity", concurrent_cross_process_integrity},
        std::pair{"corrupt event stops delivery", corrupt_event_stops_delivery},
        std::pair{"invalid configuration", invalid_configuration},
    };
    std::size_t failed = 0;
    for (const auto& [name, function] : tests) {
        try {
            function();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    return failed == 0 ? 0 : 1;
}
