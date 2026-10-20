#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace yquant::md {

enum class ErrorCode {
    InvalidArgument, SystemError, AlreadyPublished, NotFound, NotReady,
    Incompatible, Closed, EventTooLarge, BufferTooSmall, InvalidEvent,
    SequenceExhausted, InvalidGap, InvalidPosition, UnsupportedPlatform
};

struct Error {
    ErrorCode code;
    int system_errno{};
    std::string message;
};

template<class T> using Result = std::expected<T, Error>;

struct Generation {
    std::uint64_t high{};
    std::uint64_t low{};
    friend bool operator==(const Generation&, const Generation&) = default;
};

struct Endpoint {
    std::filesystem::path runtime_directory;
    std::string stream;
};

struct PublishConfig {
    Endpoint endpoint;
    std::size_t capacity{1024};
    std::size_t max_payload_size{256};
};

enum class EventScope : std::uint8_t { Business, StreamStatus, InstrumentStatus };

struct EventHeader {
    std::uint64_t source_id{};
    std::uint64_t instrument_id{};
    std::uint64_t source_timestamp_ns{};
    std::uint64_t source_sequence{};
    std::uint32_t type{};
    EventScope scope{EventScope::Business};
    bool has_source_sequence{};
};

struct EventView {
    EventHeader header;
    std::span<const std::byte> payload;
};

struct EventBuffer {
    EventHeader header;
    std::vector<std::byte> payload;
    explicit EventBuffer(std::size_t capacity = 0) { payload.reserve(capacity); }
};

// nullopt matches everything; an empty vector matches nothing.
struct Subscription {
    std::optional<std::vector<std::uint64_t>> instruments;
    std::optional<std::vector<std::uint32_t>> event_types;
};

struct StartPosition {
    enum class Mode { Live, Sequence } mode{Mode::Live};
    std::uint64_t sequence{};
    std::optional<Generation> generation;
    static StartPosition live() { return {}; }
    static StartPosition at(std::uint64_t sequence) {
        return {Mode::Sequence, sequence, std::nullopt};
    }
    static StartPosition at(Generation generation, std::uint64_t sequence) {
        return {Mode::Sequence, sequence, generation};
    }
};

struct ReadBudget {
    std::size_t max_scanned{64};
    std::size_t max_attempts{128};
};

struct GapToken {
    Generation generation;
    std::uint64_t id{};
    std::uint64_t expected_sequence{};
    std::uint64_t resume_candidate{};
    Generation subscriber_instance;
    friend bool operator==(const GapToken&, const GapToken&) = default;
};

enum class ReadStatus {
    Event, Empty, Yield, Retry, Gap, StreamChanged, EndOfGeneration, Closed, Error
};

struct ReadResult {
    ReadStatus status{ReadStatus::Empty};
    Generation generation;
    std::uint64_t sequence{};
    std::size_t scanned{};
    std::size_t attempts{};
    std::optional<GapToken> gap;
    std::optional<Error> error;
};

struct ResumePosition {
    enum class Mode { Candidate, Live, Sequence } mode{Mode::Candidate};
    std::uint64_t sequence{};
    static ResumePosition candidate() { return {}; }
    static ResumePosition live() { return {Mode::Live, 0}; }
    static ResumePosition at(std::uint64_t sequence) { return {Mode::Sequence, sequence}; }
};

enum class PublisherHealth { Ready, Stale, Stopped, Unknown };
enum class RefreshStatus { Current, StreamChanged, Missing };
struct RefreshResult {
    RefreshStatus status{RefreshStatus::Current};
    PublisherHealth health{PublisherHealth::Unknown};
    std::optional<Generation> discovered_generation;
};

class ShmPublisher {
public:
    static Result<ShmPublisher> create(const PublishConfig& config);
    ShmPublisher(ShmPublisher&&) noexcept;
    ShmPublisher& operator=(ShmPublisher&&) noexcept;
    ~ShmPublisher();
    ShmPublisher(const ShmPublisher&) = delete;
    ShmPublisher& operator=(const ShmPublisher&) = delete;

    Result<std::uint64_t> publish(EventView event);
    Result<void> tick();
    Generation generation() const noexcept;
    std::size_t capacity() const noexcept;
    std::size_t max_payload_size() const noexcept;
    void close() noexcept;

private:
    struct Impl;
    explicit ShmPublisher(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

class ShmSubscriber {
public:
    static Result<ShmSubscriber> attach(const Endpoint& endpoint,
                                      Subscription subscription = {},
                                      StartPosition start = StartPosition::live());
    ShmSubscriber(ShmSubscriber&&) noexcept;
    ShmSubscriber& operator=(ShmSubscriber&&) noexcept;
    ~ShmSubscriber();
    ShmSubscriber(const ShmSubscriber&) = delete;
    ShmSubscriber& operator=(const ShmSubscriber&) = delete;

    ReadResult try_next(EventBuffer& buffer, ReadBudget budget = {});
    Result<std::uint64_t> set_subscription(Subscription subscription);
    Result<std::uint64_t> resume(const GapToken& token, ResumePosition position = {});
    Result<RefreshResult> refresh(std::chrono::nanoseconds stale_after = std::chrono::seconds(3));
    Generation generation() const noexcept;
    std::uint64_t next_sequence() const noexcept;
    std::size_t max_payload_size() const noexcept;
    void close() noexcept;

private:
    struct Impl;
    explicit ShmSubscriber(std::unique_ptr<Impl>) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace yquant::md
