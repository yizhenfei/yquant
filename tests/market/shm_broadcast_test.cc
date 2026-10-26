#include "yquant/market/shm_broadcast.h"

#include <fcntl.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "base/internal/broadcast_layout.h"
#include "base/internal/shared_memory_mapping.h"
#include "base/internal/shm_broadcast_layout.h"
#include "base/internal/shm_stream_registry.h"
#include "file_test_support.h"
#include "gtest/gtest.h"
#include "market/internal/event_codec.h"

namespace yquant::market {
namespace {

class ShmBroadcastTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::array<char, 64> pattern{};
    std::strcpy(pattern.data(), "/tmp/yquant-md-test-XXXXXX");
    const char* directory = ::mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    endpoint_ = {directory, "ticks"};
  }

  void TearDown() override {
    auto stale = base::internal::Discover(endpoint_);
    if (stale) {
      ::shm_unlink(stale->shm_name.c_str());
    }
    std::error_code ignored;
    ::yquant::testing::files::RemoveTree(
        ::yquant::testing::files::FilePath(endpoint_.runtime_directory),
        ignored);
  }

  ShmPublisher Publisher(size_t capacity = 8, size_t max_payload_size = 64) {
    ShmPublisher publisher;
    EXPECT_TRUE(publisher.Create({endpoint_, capacity, max_payload_size}).ok());
    return publisher;
  }

  ShmReader Reader(StartPosition start = StartPosition::At(1)) {
    ShmReader reader;
    EXPECT_TRUE(reader.Open(endpoint_, start).ok());
    return reader;
  }

  Endpoint endpoint_;
};

// Fixed deterministic bytes expose mixed copies during an overlapping
// overwrite.
std::vector<std::byte> Payload(uint64_t value, size_t size = 32) {
  std::vector<std::byte> bytes(size);
  for (size_t i = 0; i < size; ++i) {
    const auto word =
        value * 0x9e3779b97f4a7c15ULL + (i / 8) * 0x517cc1b727220a95ULL;
    bytes[i] = std::byte((word >> (8 * (i % 8))) & 0xff);
  }
  return bytes;
}

EventHeader Header(uint64_t sequence, uint64_t instrument = 42,
                   uint32_t type = 7,
                   EventScope scope = EventScope::kBusiness) {
  return {13, instrument, sequence * 1000, sequence, type, scope, true};
}

void Publish(ShmPublisher& publisher, uint64_t value, uint64_t instrument = 42,
             uint32_t type = 7, EventScope scope = EventScope::kBusiness,
             size_t size = 32) {
  const auto bytes = Payload(value, size);
  uint64_t sequence = 0;
  ASSERT_TRUE(
      publisher
          .Publish({Header(value, instrument, type, scope), bytes}, &sequence)
          .ok());
  EXPECT_EQ(sequence, value);
}

void ExpectEvent(ShmReader& reader, EventBuffer& buffer, uint64_t sequence) {
  const auto result = reader.TryNext(buffer);
  ASSERT_EQ(result.status, ReadStatus::kEvent);
  EXPECT_EQ(result.sequence, sequence);
  EXPECT_EQ(result.generation, reader.generation());
  EXPECT_EQ(buffer.header.source_id, 13);
  EXPECT_EQ(buffer.header.source_sequence, sequence);
  EXPECT_EQ(buffer.header.source_timestamp_ns, sequence * 1000);
  EXPECT_TRUE(buffer.header.has_source_sequence);
  const auto expected = Payload(sequence, buffer.payload_size);
  EXPECT_TRUE(
      std::equal(expected.begin(), expected.end(), buffer.payload.begin()));
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
  template <typename Function>
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
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return status;
      }
      if (result < 0 && errno != EINTR) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
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

TEST_F(ShmBroadcastTest, MultipleReadersAndLiveStart) {
  auto publisher = Publisher(16);
  Publish(publisher, 1, 42, 7, EventScope::kBusiness, 13);
  Publish(publisher, 2, 42, 7, EventScope::kBusiness, 0);
  auto a = Reader();
  auto b = Reader();
  auto live = Reader(StartPosition::Live());
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

TEST_F(ShmBroadcastTest, DeliversEveryEventInStreamOrder) {
  auto publisher = Publisher(16);
  Publish(publisher, 1, 99, 7);
  Publish(publisher, 2, 42, 8);
  Publish(publisher, 3, 99, 99, EventScope::kStreamStatus);
  Publish(publisher, 4, 99, 99, EventScope::kInstrumentStatus);
  Publish(publisher, 5, 42, 99, EventScope::kInstrumentStatus);
  Publish(publisher, 6, 42, 7);
  auto reader = Reader();
  EventBuffer buffer;
  for (uint64_t sequence = 1; sequence <= 6; ++sequence) {
    ExpectEvent(reader, buffer, sequence);
    EXPECT_EQ(reader.next_sequence(), sequence + 1);
  }
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kEmpty);
}

TEST_F(ShmBroadcastTest, PublicationErrorsAndUnchangedReadBuffer) {
  auto publisher = Publisher(8, 32);
  const auto bytes = Payload(1, 33);
  uint64_t sequence = 999;
  EXPECT_EQ(publisher.Publish({Header(1), bytes}, &sequence).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(sequence, 999);
  auto invalid = Header(1);
  invalid.scope = static_cast<EventScope>(255);
  EXPECT_FALSE(publisher.Publish({invalid, {}}).ok());
  Publish(publisher, 1);
  auto reader = Reader();
  EventBuffer buffer;
  buffer.payload_size = 7;
  EXPECT_EQ(reader.TryNext(buffer, {0}).error.code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(buffer.payload_size, 7);
  EXPECT_EQ(reader.next_sequence(), 1);
  ExpectEvent(reader, buffer, 1);
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kEmpty);
  EXPECT_EQ(buffer.header.source_sequence, 1);
  reader.Close();
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kClosed);
}

TEST_F(ShmBroadcastTest, OverrunAndExplicitRecovery) {
  auto publisher = Publisher(4);
  auto reader = Reader();
  for (uint64_t sequence = 1; sequence <= 8; ++sequence) {
    Publish(publisher, sequence);
  }
  EventBuffer buffer;
  const auto result = reader.TryNext(buffer);
  ASSERT_EQ(result.status, ReadStatus::kGap);
  ASSERT_TRUE(result.gap);
  const auto gap = *result.gap;
  EXPECT_EQ(gap.expected_sequence, 1);
  EXPECT_EQ(gap.resume_candidate, 2);
  EXPECT_EQ(reader.next_sequence(), 1);
  EXPECT_EQ(reader.TryNext(buffer).gap, gap);
  uint64_t sequence = 99;
  auto forged = gap;
  ++forged.id;
  EXPECT_FALSE(reader.Resume(forged, sequence).ok());
  EXPECT_FALSE(reader.Resume(gap, sequence, ResumePosition::At(0)).ok());
  auto other = Reader();
  EXPECT_EQ(other.TryNext(buffer).status, ReadStatus::kGap);
  EXPECT_FALSE(other.Resume(gap, sequence).ok());
  ASSERT_TRUE(reader.Resume(gap, sequence).ok());
  EXPECT_EQ(sequence, 2);
  auto again = reader.TryNext(buffer);
  ASSERT_TRUE(again.gap);
  ASSERT_TRUE(reader.Resume(*again.gap, sequence, ResumePosition::At(5)).ok());
  ExpectEvent(reader, buffer, 5);
  EXPECT_FALSE(reader.Resume(gap, sequence).ok());
  for (uint64_t next = 9; next <= 13; ++next) {
    Publish(publisher, next);
  }
  again = reader.TryNext(buffer);
  ASSERT_TRUE(again.gap);
  EXPECT_NE(again.gap->id, gap.id);
  EXPECT_EQ(again.gap->expected_sequence, 6);
  ASSERT_TRUE(reader.Resume(*again.gap, sequence, ResumePosition::Live()).ok());
  EXPECT_EQ(sequence, 14);
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kEmpty);
  Publish(publisher, 14);
  ExpectEvent(reader, buffer, 14);
}

TEST_F(ShmBroadcastTest, StoppedGenerationDrains) {
  auto publisher = Publisher();
  auto reader = Reader();
  Publish(publisher, 1);
  Publish(publisher, 2);
  publisher.Close();
  EventBuffer buffer;
  ExpectEvent(reader, buffer, 1);
  ExpectEvent(reader, buffer, 2);
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kEndOfGeneration);
  EXPECT_FALSE(publisher.Publish({Header(3), {}}).ok());
  EXPECT_FALSE(publisher.Tick().ok());
}

TEST_F(ShmBroadcastTest, RefreshAndRestartBoundaries) {
  auto publisher = Publisher(2);
  const auto generation = publisher.generation();
  auto reader = Reader();
  for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
    Publish(publisher, sequence);
  }
  EventBuffer buffer;
  const auto result = reader.TryNext(buffer);
  ASSERT_TRUE(result.gap);
  const auto gap = *result.gap;
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  RefreshResult refresh;
  ASSERT_TRUE(reader.Refresh(refresh, std::chrono::nanoseconds(1)).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kCurrent);
  EXPECT_EQ(refresh.health, PublisherHealth::kStale);
  ASSERT_TRUE(publisher.Tick().ok());
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.health, PublisherHealth::kReady);
  publisher.Close();
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kMissing);
  auto replacement = Publisher();
  EXPECT_NE(replacement.generation(), generation);
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
  EXPECT_EQ(refresh.discovered_generation, replacement.generation());
  EXPECT_EQ(reader.TryNext(buffer).status, ReadStatus::kStreamChanged);
  uint64_t sequence = 0;
  EXPECT_FALSE(reader.Resume(gap, sequence).ok());
  auto reconnected = Reader();
  EXPECT_FALSE(reconnected.Resume(gap, sequence).ok());
  Publish(replacement, 1);
  ExpectEvent(reconnected, buffer, 1);
  ShmReader old_generation;
  EXPECT_FALSE(
      old_generation.Open(endpoint_, StartPosition::At(generation, 1)).ok());
  replacement.Close();
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmBroadcastTest, LockSurvivesInheritedClose) {
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

TEST_F(ShmBroadcastTest, AbruptExitReleasesLock) {
  Pipe ready;
  Child child([&] {
    auto publisher = Publisher();
    Publish(publisher, 1);
    ready.Signal();
    std::this_thread::sleep_for(std::chrono::seconds(8));
    FAIL() << "expected SIGKILL";
  });
  ready.Wait();
  auto reader = Reader();
  const auto generation = reader.generation();
  child.Kill();
  const int child_status = child.Wait();
  EXPECT_TRUE(WIFSIGNALED(child_status));
  auto replacement = Publisher();
  EXPECT_NE(replacement.generation(), generation);
  RefreshResult refresh;
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmBroadcastTest, PendingInitializationRecovery) {
  Pipe ready;
  Child child([&] {
    auto publisher = Publisher();
    Publish(publisher, 1);
    ready.Signal();
    std::this_thread::sleep_for(std::chrono::seconds(8));
    FAIL() << "expected initialization crash";
  });
  ready.Wait();
  auto reader = Reader();
  auto previous = base::internal::Discover(endpoint_);
  ASSERT_TRUE(previous);
  std::error_code error;
  ::yquant::testing::files::Rename(
      ::yquant::testing::files::FilePath(
          ::yquant::testing::files::FilePath(endpoint_.runtime_directory) /
          "ticks.manifest"),
      ::yquant::testing::files::FilePath(
          ::yquant::testing::files::FilePath(endpoint_.runtime_directory) /
          "ticks.pending"),
      error);
  ASSERT_FALSE(error);
  child.Kill();
  const int child_status = child.Wait();
  EXPECT_TRUE(WIFSIGNALED(child_status));
  auto replacement = Publisher();
  EXPECT_FALSE(
      ::yquant::testing::files::Exists(::yquant::testing::files::FilePath(
          ::yquant::testing::files::FilePath(endpoint_.runtime_directory) /
          "ticks.pending")));
  const int stale_fd = ::shm_open(previous->shm_name.c_str(), O_RDWR, 0);
  const int stale_errno = errno;
  if (stale_fd >= 0) {
    ::close(stale_fd);
  }
  EXPECT_LT(stale_fd, 0);
  EXPECT_EQ(stale_errno, ENOENT);
  RefreshResult refresh;
  ASSERT_TRUE(reader.Refresh(refresh).ok());
  EXPECT_EQ(refresh.status, RefreshStatus::kStreamChanged);
}

TEST_F(ShmBroadcastTest, CrossProcessOverwriteIntegrity) {
  auto publisher = Publisher(16, 512);
  Pipe ready;
  Child child([&] {
    publisher.Close();
    auto reader = Reader();
    EventBuffer buffer;
    ready.Signal();
    uint64_t previous = 0;
    size_t received = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = reader.TryNext(buffer, {32});
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
        uint64_t sequence = 0;
        ASSERT_TRUE(reader.Resume(*result.gap, sequence).ok());
      } else if (result.status == ReadStatus::kEndOfGeneration) {
        EXPECT_GT(received, 0);
        return;
      } else {
        ASSERT_TRUE(result.status == ReadStatus::kEmpty ||
                    result.status == ReadStatus::kRetry);
      }
    }
    FAIL() << "stress reader timed out";
  });
  ready.Wait();
  for (uint64_t sequence = 1; sequence <= 50'000; ++sequence) {
    Publish(publisher, sequence, sequence & 7, 7, EventScope::kBusiness, 512);
  }
  publisher.Close();
  child.Success();
}

TEST_F(ShmBroadcastTest, CorruptEventStopsDelivery) {
  base::ShmBroadcastWriter writer;
  ASSERT_TRUE(writer.Create({endpoint_, {8, 80}}).ok());
  const auto bytes = Payload(1, 8);
  std::array<uint64_t, 10> encoded{};
  ASSERT_TRUE(internal::EncodeEvent({Header(1), bytes}, encoded, 16));
  const auto valid = encoded;
  auto wire = std::as_writable_bytes(std::span(encoded));
  std::fill(wire.begin() + 40, wire.begin() + 44, std::byte{0xff});
  ASSERT_TRUE(writer.TryWrite(std::as_bytes(std::span(encoded))).ok());
  auto reader = Reader();
  EventBuffer output;
  output.payload_size = 7;
  const auto malformed = reader.TryNext(output);
  EXPECT_EQ(malformed.status, ReadStatus::kError);
  EXPECT_EQ(malformed.error.code, base::ErrorCode::kCorrupt);
  EXPECT_EQ(reader.next_sequence(), 1);
  EXPECT_EQ(output.payload_size, 7);
  // Repair bytes under exclusive test access; the failed reader stays stopped.
  auto manifest = base::internal::Discover(endpoint_);
  ASSERT_TRUE(manifest);
  base::internal::SharedMemoryMapping mapping;
  ASSERT_TRUE(mapping.Open(manifest->shm_name).ok());
  auto memory =
      mapping.memory().subspan(sizeof(base::internal::ShmBroadcastControl));
  auto* header =
      reinterpret_cast<base::internal::BroadcastHeader*>(memory.data());
  const auto& layout = header->layout;
  for (size_t i = 0; i < valid.size(); ++i) {
    base::internal::BroadcastSlot(memory.data() + layout.slots_offset,
                                  layout.slot_stride, layout.capacity, 1, i + 2)
        .store(valid[i], std::memory_order_relaxed);
  }
  EXPECT_EQ(reader.TryNext(output).error.code, base::ErrorCode::kCorrupt);
  auto fresh = Reader();
  ExpectEvent(fresh, output, 1);
}

TEST_F(ShmBroadcastTest, InvalidConfiguration) {
  ShmPublisher publisher;
  EXPECT_FALSE(publisher.Create({endpoint_, 0, 64}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 3, 64}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 8, kMaxEventPayloadSize + 1}).ok());
  EXPECT_FALSE(
      publisher.Create({endpoint_, 8, std::numeric_limits<size_t>::max()})
          .ok());
  EXPECT_FALSE(
      publisher.Create({{endpoint_.runtime_directory, "../escape"}, 8, 64})
          .ok());
  ShmReader missing;
  EXPECT_EQ(missing.Open(endpoint_).system_error.value(), ENOENT);
  ASSERT_TRUE(publisher.Create({endpoint_, 8, kMaxEventPayloadSize}).ok());
  EXPECT_FALSE(publisher.Create({endpoint_, 8, 64}).ok());
  auto reader = Reader();
  const auto bytes = Payload(1, kMaxEventPayloadSize);
  ASSERT_TRUE(publisher.Publish({Header(1), bytes}).ok());
  EventBuffer buffer;
  ExpectEvent(reader, buffer, 1);
  EXPECT_EQ(buffer.payload_size, kMaxEventPayloadSize);
}

}  // namespace
}  // namespace yquant::market
