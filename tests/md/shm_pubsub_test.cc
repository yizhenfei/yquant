#include "yquant/md/shm_pubsub.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "md/internal/event_codec.h"
#include "md/internal/shm_segment.h"

namespace yquant::md {
namespace {

using namespace std::chrono_literals;

class ShmPubsubTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::array<char, 64> pattern{};
    std::strcpy(pattern.data(), "/tmp/yquant-md-test-XXXXXX");
    const char* directory = ::mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    endpoint_ = {directory, "ticks"};
  }

  void TearDown() override {
    auto stale = internal::Discover(endpoint_);
    if (stale) ::shm_unlink(stale->shm_name.c_str());
    std::error_code ignored;
    std::filesystem::remove_all(endpoint_.runtime_directory, ignored);
  }

  ShmPublisher Publisher(std::size_t capacity = 8,
                         std::size_t max_payload_size = 64) {
    ShmPublisher publisher;
    EXPECT_TRUE(publisher.Create({endpoint_, capacity, max_payload_size}).ok());
    return publisher;
  }

  ShmSubscriber Subscriber(StartPosition start = StartPosition::At(1),
                           Subscription filter = {}) {
    ShmSubscriber subscriber;
    EXPECT_TRUE(subscriber.Open(endpoint_, filter, start).ok());
    return subscriber;
  }

  Endpoint endpoint_;
};

// Fixed deterministic bytes expose mixed copies during an overlapping
// overwrite.
std::vector<std::byte> Payload(std::uint64_t value, std::size_t size = 32) {
  std::vector<std::byte> bytes(size);
  for (std::size_t i = 0; i < size; ++i) {
    const auto word =
        value * 0x9e3779b97f4a7c15ULL + (i / 8) * 0x517cc1b727220a95ULL;
    bytes[i] = std::byte((word >> (8 * (i % 8))) & 0xff);
  }
  return bytes;
}

EventHeader Header(std::uint64_t sequence, std::uint64_t instrument = 42,
                   std::uint32_t type = 7,
                   EventScope scope = EventScope::kBusiness) {
  return {13, instrument, sequence * 1000, sequence, type, scope, true};
}

void Publish(ShmPublisher& publisher, std::uint64_t value,
             std::uint64_t instrument = 42, std::uint32_t type = 7,
             EventScope scope = EventScope::kBusiness, std::size_t size = 32) {
  const auto bytes = Payload(value, size);
  std::uint64_t sequence = 0;
  ASSERT_TRUE(
      publisher
          .Publish({Header(value, instrument, type, scope), bytes}, &sequence)
          .ok());
  EXPECT_EQ(sequence, value);
}

void ExpectEvent(ShmSubscriber& subscriber, EventBuffer& buffer,
                 std::uint64_t sequence) {
  const auto result = subscriber.TryNext(buffer);
  ASSERT_EQ(result.status, ReadStatus::kEvent);
  EXPECT_EQ(result.sequence, sequence);
  EXPECT_EQ(result.generation, subscriber.generation());
  EXPECT_EQ(buffer.header.source_id, 13);
  EXPECT_EQ(buffer.header.source_sequence, sequence);
  EXPECT_EQ(buffer.header.source_timestamp_ns, sequence * 1000);
  EXPECT_TRUE(buffer.header.has_source_sequence);
  const auto expected = Payload(sequence, buffer.payload_size);
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(), buffer.payload.begin()));
}

Subscription Filter(std::initializer_list<std::uint64_t> instruments,
                    std::initializer_list<std::uint32_t> types = {},
                    bool all_types = true) {
  Subscription filter;
  filter.all_instruments = false;
  filter.instrument_count = instruments.size();
  std::copy(instruments.begin(), instruments.end(), filter.instruments.begin());
  filter.all_event_types = all_types;
  filter.event_type_count = types.size();
  std::copy(types.begin(), types.end(), filter.event_types.begin());
  return filter;
}

class Pipe {
 public:
  Pipe() { EXPECT_EQ(::pipe(fds_), 0); }
  ~Pipe() {
    ::close(fds_[0]);
    ::close(fds_[1]);
  }
  void Signal() {
    const char byte = 1;
    ASSERT_EQ(::write(fds_[1], &byte, 1), 1);
  }
  void Wait() {
    pollfd descriptor{fds_[0], POLLIN, 0};
    ASSERT_EQ(::poll(&descriptor, 1, 5000), 1);
    char byte;
    ASSERT_EQ(::read(fds_[0], &byte, 1), 1);
  }

 private:
  int fds_[2] = {-1, -1};
};

// Child-side GoogleTest failures become a nonzero exit status in the parent.
class Child {
 public:
  template <class Function>
  explicit Child(Function function) {
    pid_ = ::fork();
    EXPECT_GE(pid_, 0);
    if (pid_ == 0) {
      function();
      ::_exit(::testing::Test::HasFailure() ? 1 : 0);
    }
  }
  ~Child() {
    if (pid_ > 0) {
      ::kill(pid_, SIGKILL);
      int status;
      while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  void Kill() { ASSERT_EQ(::kill(pid_, SIGKILL), 0); }
  int Wait() {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return status;
      }
      if (result < 0 && errno != EINTR) break;
      std::this_thread::sleep_for(1ms);
    }
    ADD_FAILURE() << "child wait timed out";
    return -1;
  }
  void Success() {
    const auto status = Wait();
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

 private:
  pid_t pid_ = -1;
};

TEST_F(ShmPubsubTest, MultipleReadersAndLiveStart) {
  auto publisher = Publisher(16);
  Publish(publisher, 1, 42, 7, EventScope::kBusiness, 13);
  Publish(publisher, 2, 42, 7, EventScope::kBusiness, 0);
  auto a = Subscriber();
  auto b = Subscriber();
  auto live = Subscriber(StartPosition::Live());
  EXPECT_EQ(live.next_sequence(), 3);
  EventBuffer buffer;
  EXPECT_EQ(live.TryNext(buffer).status, ReadStatus::kEmpty);
  ExpectEvent(a, buffer, 1);
  ExpectEvent(a, buffer, 2);
  ExpectEvent(b, buffer, 1);
  Publish(publisher, 3);
  ExpectEvent(live, buffer, 3);
  ExpectEvent(b, buffer, 2);
  ExpectEvent(b, buffer, 3);
  ExpectEvent(a, buffer, 3);
  EXPECT_EQ(a.TryNext(buffer).status, ReadStatus::kEmpty);
}

TEST_F(ShmPubsubTest, LocalFilteringAndBoundedScanning) {
  auto publisher = Publisher(16);
  Publish(publisher, 1, 99, 7);
  Publish(publisher, 2, 42, 8);
  Publish(publisher, 3, 99, 99, EventScope::kStreamStatus);
  Publish(publisher, 4, 99, 99, EventScope::kInstrumentStatus);
  Publish(publisher, 5, 42, 99, EventScope::kInstrumentStatus);
  Publish(publisher, 6, 42, 7);
  auto subscriber = Subscriber(StartPosition::At(1), Filter({42}, {7}, false));
  EventBuffer buffer;
  auto result = subscriber.TryNext(buffer, {1, 8});
  EXPECT_EQ(result.status, ReadStatus::kYield);
  EXPECT_EQ(result.scanned, 1);
  EXPECT_EQ(subscriber.next_sequence(), 2);
  EXPECT_EQ(subscriber.TryNext(buffer, {1, 8}).status, ReadStatus::kYield);
  ExpectEvent(subscriber, buffer, 3);
  ExpectEvent(subscriber, buffer, 5);
  ExpectEvent(subscriber, buffer, 6);
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kEmpty);
  auto empty = Subscriber(StartPosition::At(1), Filter({}));
  ExpectEvent(empty, buffer, 3);
  EXPECT_EQ(empty.TryNext(buffer).status, ReadStatus::kEmpty);
}

TEST_F(ShmPubsubTest, SubscriptionEffectiveBoundary) {
  auto publisher = Publisher();
  Publish(publisher, 1, 2);
  Publish(publisher, 2, 1);
  Publish(publisher, 3, 2);
  auto subscriber = Subscriber(StartPosition::At(1), Filter({1}));
  EventBuffer buffer;
  EXPECT_EQ(subscriber.TryNext(buffer, {1, 8}).status, ReadStatus::kYield);
  std::uint64_t boundary = 0;
  ASSERT_TRUE(subscriber.SetSubscription(Filter({2}), boundary).ok());
  EXPECT_EQ(boundary, 2);
  ExpectEvent(subscriber, buffer, 3);
  EXPECT_EQ(subscriber.next_sequence(), 4);
  auto invalid = Filter({2});
  invalid.instrument_count = kMaxSubscriptionEntries + 1;
  EXPECT_FALSE(subscriber.SetSubscription(invalid, boundary).ok());
  EXPECT_EQ(boundary, 2);
}

TEST_F(ShmPubsubTest, PublicationErrorsAndUnchangedReadBuffer) {
  auto publisher = Publisher(8, 32);
  const auto bytes = Payload(1, 33);
  std::uint64_t sequence = 999;
  EXPECT_EQ(publisher.Publish({Header(1), bytes}, &sequence).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(sequence, 999);
  auto invalid = Header(1);
  invalid.scope = static_cast<EventScope>(255);
  EXPECT_FALSE(publisher.Publish({invalid, {}}).ok());
  Publish(publisher, 1);
  auto subscriber = Subscriber();
  EventBuffer buffer;
  buffer.payload_size = 7;
  EXPECT_EQ(subscriber.TryNext(buffer, {0, 1}).error.code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(buffer.payload_size, 7);
  EXPECT_EQ(subscriber.next_sequence(), 1);
  ExpectEvent(subscriber, buffer, 1);
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kEmpty);
  EXPECT_EQ(buffer.header.source_sequence, 1);
  subscriber.Close();
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kClosed);
}

TEST_F(ShmPubsubTest, OverrunAndExplicitRecovery) {
  auto publisher = Publisher(4);
  auto subscriber = Subscriber();
  for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) {
    Publish(publisher, sequence);
  }
  EventBuffer buffer;
  const auto result = subscriber.TryNext(buffer);
  ASSERT_EQ(result.status, ReadStatus::kGap);
  ASSERT_TRUE(result.gap);
  const auto gap = *result.gap;
  EXPECT_EQ(gap.expected_sequence, 1);
  EXPECT_EQ(gap.resume_candidate, 2);
  EXPECT_EQ(subscriber.next_sequence(), 1);
  EXPECT_EQ(subscriber.TryNext(buffer).gap, gap);
  std::uint64_t sequence = 99;
  ASSERT_TRUE(subscriber.SetSubscription({}, sequence).ok());
  EXPECT_EQ(sequence, 1);
  EXPECT_EQ(subscriber.TryNext(buffer).gap, gap);
  auto forged = gap;
  ++forged.id;
  EXPECT_FALSE(subscriber.Resume(forged, sequence).ok());
  EXPECT_FALSE(subscriber.Resume(gap, sequence, ResumePosition::At(0)).ok());
  auto other = Subscriber();
  EXPECT_EQ(other.TryNext(buffer).status, ReadStatus::kGap);
  EXPECT_FALSE(other.Resume(gap, sequence).ok());
  ASSERT_TRUE(subscriber.Resume(gap, sequence).ok());
  EXPECT_EQ(sequence, 2);
  auto again = subscriber.TryNext(buffer);
  ASSERT_TRUE(again.gap);
  ASSERT_TRUE(
      subscriber.Resume(*again.gap, sequence, ResumePosition::At(5)).ok());
  ExpectEvent(subscriber, buffer, 5);
  EXPECT_FALSE(subscriber.Resume(gap, sequence).ok());
  for (std::uint64_t next = 9; next <= 13; ++next) Publish(publisher, next);
  again = subscriber.TryNext(buffer);
  ASSERT_TRUE(again.gap);
  EXPECT_NE(again.gap->id, gap.id);
  EXPECT_EQ(again.gap->expected_sequence, 6);
  ASSERT_TRUE(
      subscriber.Resume(*again.gap, sequence, ResumePosition::Live()).ok());
  EXPECT_EQ(sequence, 14);
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kEmpty);
  Publish(publisher, 14);
  ExpectEvent(subscriber, buffer, 14);
}

TEST_F(ShmPubsubTest, StoppedGenerationDrains) {
  auto publisher = Publisher();
  auto subscriber = Subscriber();
  Publish(publisher, 1);
  Publish(publisher, 2);
  publisher.Close();
  EventBuffer buffer;
  ExpectEvent(subscriber, buffer, 1);
  ExpectEvent(subscriber, buffer, 2);
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kEndOfGeneration);
  EXPECT_FALSE(publisher.Publish({Header(3), {}}).ok());
  EXPECT_FALSE(publisher.Tick().ok());
}

TEST_F(ShmPubsubTest, RefreshAndRestartBoundaries) {
  auto publisher = Publisher(2);
  const auto generation = publisher.generation();
  auto subscriber = Subscriber();
  for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
    Publish(publisher, sequence);
  }
  EventBuffer buffer;
  const auto result = subscriber.TryNext(buffer);
  ASSERT_TRUE(result.gap);
  const auto gap = *result.gap;
  std::this_thread::sleep_for(2ms);
  RefreshResult refresh;
  ASSERT_TRUE(subscriber.Refresh(refresh, 1ns).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kCurrent);
  EXPECT_EQ(refresh.health, PublisherHealth::kStale);
  ASSERT_TRUE(publisher.Tick().ok());
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.health, PublisherHealth::kReady);
  publisher.Close();
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kMissing);
  auto replacement = Publisher();
  EXPECT_NE(replacement.generation(), generation);
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
  EXPECT_EQ(refresh.discovered_generation, replacement.generation());
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kStreamChanged);
  std::uint64_t sequence = 0;
  EXPECT_FALSE(subscriber.Resume(gap, sequence).ok());
  auto reconnected = Subscriber();
  EXPECT_FALSE(reconnected.Resume(gap, sequence).ok());
  Publish(replacement, 1);
  ExpectEvent(reconnected, buffer, 1);
  ShmSubscriber old_generation;
  EXPECT_FALSE(
      old_generation.Open(endpoint_, {}, StartPosition::At(generation, 1))
          .ok());
  replacement.Close();
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmPubsubTest, LockSurvivesInheritedClose) {
  auto publisher = Publisher();
  ShmPublisher duplicate;
  EXPECT_EQ(duplicate.Create({endpoint_, 8, 64}).code,
            base::ErrorCode::kInvalidState);
  Child child([&] {
    EXPECT_FALSE(publisher.Publish({Header(1), {}}).ok());
    EXPECT_FALSE(publisher.Tick().ok());
    publisher.Close();
    ShmPublisher attempted;
    EXPECT_EQ(attempted.Create({endpoint_, 8, 64}).code,
              base::ErrorCode::kInvalidState);
  });
  child.Success();
  Publish(publisher, 1);
  EXPECT_FALSE(duplicate.Create({endpoint_, 8, 64}).ok());
  publisher.Close();
  ASSERT_TRUE(duplicate.Create({endpoint_, 8, 64}).ok());
  Publish(duplicate, 1);
}

TEST_F(ShmPubsubTest, AbruptExitReleasesLock) {
  Pipe ready;
  Child child([&] {
    auto publisher = Publisher();
    Publish(publisher, 1);
    ready.Signal();
    std::this_thread::sleep_for(8s);
    FAIL() << "expected SIGKILL";
  });
  ready.Wait();
  auto subscriber = Subscriber();
  const auto generation = subscriber.generation();
  child.Kill();
  const int child_status = child.Wait();
  EXPECT_TRUE(WIFSIGNALED(child_status));
  auto replacement = Publisher();
  EXPECT_NE(replacement.generation(), generation);
  RefreshResult refresh;
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmPubsubTest, PendingInitializationRecovery) {
  Pipe ready;
  Child child([&] {
    auto publisher = Publisher();
    Publish(publisher, 1);
    ready.Signal();
    std::this_thread::sleep_for(8s);
    FAIL() << "expected initialization crash";
  });
  ready.Wait();
  auto subscriber = Subscriber();
  auto previous = internal::Discover(endpoint_);
  ASSERT_TRUE(previous);
  std::error_code error;
  std::filesystem::rename(endpoint_.runtime_directory / "ticks.manifest",
                          endpoint_.runtime_directory / "ticks.pending", error);
  ASSERT_FALSE(error);
  child.Kill();
  const int child_status = child.Wait();
  EXPECT_TRUE(WIFSIGNALED(child_status));
  auto replacement = Publisher();
  EXPECT_FALSE(
      std::filesystem::exists(endpoint_.runtime_directory / "ticks.pending"));
  const int stale_fd = ::shm_open(previous->shm_name.c_str(), O_RDWR, 0);
  const int stale_errno = errno;
  if (stale_fd >= 0) ::close(stale_fd);
  EXPECT_LT(stale_fd, 0);
  EXPECT_EQ(stale_errno, ENOENT);
  RefreshResult refresh;
  ASSERT_TRUE(subscriber.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmPubsubTest, AtomicsAliasAtDistinctAddresses) {
  auto owner = internal::Segment::Create({endpoint_, 8, 64});
  ASSERT_TRUE(owner);
  auto alias = internal::Segment::Attach(endpoint_);
  ASSERT_TRUE(alias);
  EXPECT_NE(&(*owner)->header(), &(*alias)->header());
  EXPECT_NE(&(*owner)->word(1, 0), &(*alias)->word(1, 0));
  (*owner)->word(1, 0).store(0x123456789abcdef0ULL, std::memory_order_release);
  EXPECT_EQ((*alias)->word(1, 0).load(std::memory_order_acquire),
            0x123456789abcdef0ULL);
  Child child([&] {
    auto independent = internal::Segment::Attach(endpoint_);
    ASSERT_TRUE(independent);
    EXPECT_NE(&(*independent)->header(), &(*owner)->header());
    EXPECT_NE(&(*independent)->header(), &(*alias)->header());
    EXPECT_EQ((*independent)->word(1, 0).load(std::memory_order_acquire),
              0x123456789abcdef0ULL);
    (*independent)
        ->word(1, 0)
        .store(0xfedcba9876543210ULL, std::memory_order_release);
  });
  child.Success();
  EXPECT_EQ((*owner)->word(1, 0).load(std::memory_order_acquire),
            0xfedcba9876543210ULL);
}

TEST_F(ShmPubsubTest, StalledSlotAndPrecommitOverwrite) {
  auto segment = internal::Segment::Create({endpoint_, 4, 32});
  ASSERT_TRUE(segment);
  auto subscriber = Subscriber();
  EventBuffer buffer;
  EXPECT_EQ(subscriber.TryNext(buffer).status, ReadStatus::kEmpty);
  (*segment)->header().published_sequence.store(1, std::memory_order_release);
  (*segment)->stamp(1).store(1, std::memory_order_release);
  const auto stalled = subscriber.TryNext(buffer, {8, 3});
  EXPECT_EQ(stalled.status, ReadStatus::kRetry);
  EXPECT_EQ(stalled.attempts, 3);
  EXPECT_EQ(subscriber.next_sequence(), 1);
  (*segment)->header().published_sequence.store(4, std::memory_order_release);
  (*segment)->stamp(5).store(9, std::memory_order_release);
  const auto overwritten = subscriber.TryNext(buffer);
  ASSERT_EQ(overwritten.status, ReadStatus::kGap);
  ASSERT_TRUE(overwritten.gap);
  EXPECT_EQ(overwritten.gap->expected_sequence, 1);
  EXPECT_EQ(overwritten.gap->resume_candidate, 2);
}

TEST_F(ShmPubsubTest, RejectUntrustedLayout) {
  auto segment = internal::Segment::Create({endpoint_, 8, 64});
  ASSERT_TRUE(segment);
  auto& bootstrap = (*segment)->header().bootstrap;
  const auto original = bootstrap;
  auto rejected = [&] {
    ShmSubscriber subscriber;
    EXPECT_EQ(subscriber.Open(endpoint_).code, base::ErrorCode::kCorrupt);
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
  const auto manifest = internal::Discover(endpoint_);
  ASSERT_TRUE(manifest);
  ASSERT_EQ(::shm_unlink(manifest->shm_name.c_str()), 0);
  const int fd =
      ::shm_open(manifest->shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  ASSERT_GE(fd, 0);
  rejected();
  EXPECT_EQ(::ftruncate(fd, 8), 0);
  ::close(fd);
  rejected();
}

TEST_F(ShmPubsubTest, CrossProcessOverwriteIntegrity) {
  auto publisher = Publisher(16, 512);
  Pipe ready;
  Child child([&] {
    publisher.Close();
    auto subscriber = Subscriber();
    EventBuffer buffer;
    ready.Signal();
    std::uint64_t previous = 0;
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = subscriber.TryNext(buffer, {16, 32});
      if (result.status == ReadStatus::kEvent) {
        ASSERT_GT(result.sequence, previous);
        EXPECT_EQ(buffer.header.source_sequence, result.sequence);
        EXPECT_EQ(buffer.header.instrument_id, result.sequence & 7);
        EXPECT_EQ(buffer.header.source_timestamp_ns, result.sequence * 1000);
        ASSERT_EQ(buffer.payload_size, 512);
        const auto expected = Payload(result.sequence, 512);
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(),
                               buffer.payload.begin()));
        previous = result.sequence;
        ++received;
      } else if (result.status == ReadStatus::kGap) {
        ASSERT_TRUE(result.gap);
        std::uint64_t sequence = 0;
        ASSERT_TRUE(subscriber.Resume(*result.gap, sequence).ok());
      } else if (result.status == ReadStatus::kEndOfGeneration) {
        EXPECT_GT(received, 0);
        return;
      } else {
        ASSERT_TRUE(result.status == ReadStatus::kEmpty ||
                    result.status == ReadStatus::kRetry ||
                    result.status == ReadStatus::kYield);
      }
    }
    FAIL() << "stress reader timed out";
  });
  ready.Wait();
  for (std::uint64_t sequence = 1; sequence <= 50'000; ++sequence) {
    Publish(publisher, sequence, sequence & 7, 7, EventScope::kBusiness, 512);
  }
  publisher.Close();
  child.Success();
}

TEST_F(ShmPubsubTest, CorruptEventStopsDelivery) {
  auto segment = internal::Segment::Create({endpoint_, 8, 16});
  ASSERT_TRUE(segment);
  const auto bytes = Payload(1, 8);
  std::array<std::uint64_t, 10> encoded{};
  ASSERT_EQ(encoded.size(), (*segment)->layout().word_count);
  ASSERT_TRUE(internal::EncodeEvent({Header(1), bytes}, encoded, 16));
  const auto valid = encoded;
  auto wire = std::as_writable_bytes(std::span(encoded));
  std::fill(wire.begin() + 40, wire.begin() + 44, std::byte{0xff});
  for (std::size_t i = 0; i < encoded.size(); ++i) {
    (*segment)->word(1, i).store(encoded[i], std::memory_order_relaxed);
  }
  (*segment)->stamp(1).store(2, std::memory_order_release);
  (*segment)->header().published_sequence.store(1, std::memory_order_release);
  auto subscriber = Subscriber();
  EventBuffer output;
  const auto malformed = subscriber.TryNext(output);
  EXPECT_EQ(malformed.status, ReadStatus::kError);
  EXPECT_EQ(malformed.error.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(subscriber.next_sequence(), 1);
  for (std::size_t i = 0; i < valid.size(); ++i) {
    (*segment)->word(1, i).store(valid[i], std::memory_order_relaxed);
  }
  EXPECT_EQ(subscriber.TryNext(output).error.code, base::ErrorCode::kCorrupt);
  auto fresh = Subscriber();
  ExpectEvent(fresh, output, 1);
}

TEST_F(ShmPubsubTest, InvalidConfiguration) {
  ShmPublisher publisher;
  EXPECT_FALSE(publisher.Create({endpoint_, 0, 64}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 3, 64}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 8, kMaxEventPayloadSize + 1}).ok());
  EXPECT_FALSE(
      publisher.Create({endpoint_, 8, std::numeric_limits<std::size_t>::max()})
          .ok());
  EXPECT_FALSE(
      publisher.Create({{endpoint_.runtime_directory, "../escape"}, 8, 64})
          .ok());
  ShmSubscriber missing;
  EXPECT_EQ(missing.Open(endpoint_).system_error.value(), ENOENT);
  ASSERT_TRUE(publisher.Create({endpoint_, 8, kMaxEventPayloadSize}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 8, 64}).ok());
  auto subscriber = Subscriber();
  const auto bytes = Payload(1, kMaxEventPayloadSize);
  ASSERT_TRUE(publisher.Publish({Header(1), bytes}).ok());
  EventBuffer buffer;
  ExpectEvent(subscriber, buffer, 1);
  EXPECT_EQ(buffer.payload_size, kMaxEventPayloadSize);
}

}  // namespace
}  // namespace yquant::md
