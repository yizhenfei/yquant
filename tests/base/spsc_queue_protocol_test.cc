#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <span>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;

TEST(SpscQueueWrapTest, ContiguousFramesSkipExplicitAndImplicitTailPadding) {
  for (std::size_t tail : {1U, 2U, 3U, 4U, 7U}) {
    SCOPED_TRACE(tail);
    alignas(128) std::array<std::byte, 400> memory{};
    ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
    SpscQueue producer;
    SpscQueue consumer;
    ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
    ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
    std::array<std::byte, 16> output{};
    // Advance an empty queue to a position with the selected tail length.
    const std::size_t first_payload_size = 16 - tail - 4;
    ASSERT_EQ(producer.TryWrite(std::span(output).first(first_payload_size)),
              SpscStatus::kSuccess);
    ASSERT_EQ(consumer.TryRead(output).status, SpscStatus::kSuccess);
    const std::size_t payload_size = 16 - tail - 4;
    auto reservation = producer.TryReserve(payload_size);
    ASSERT_EQ(reservation.status, SpscStatus::kSuccess);
    // Frame header and payload begin at the ring head after the tail jump.
    EXPECT_EQ(reservation.reservation.payload().data(), memory.data() + 388);
    std::fill(reservation.reservation.payload().begin(),
              reservation.reservation.payload().end(), std::byte{0x6e});
    ASSERT_EQ(reservation.reservation.Commit(payload_size),
              SpscStatus::kSuccess);
    auto acquired = consumer.TryAcquire();
    ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
    EXPECT_EQ(acquired.lease.payload().data(), memory.data() + 388);
    EXPECT_EQ(acquired.lease.size(), payload_size);
    EXPECT_TRUE(std::all_of(
        acquired.lease.payload().begin(), acquired.lease.payload().end(),
        [](std::byte value) { return value == std::byte{0x6e}; }));
    ASSERT_EQ(acquired.lease.Release(), SpscStatus::kSuccess);
    EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
  }
}

TEST(SpscQueueWrapTest, FullSizeFrameMakesProgressFromEveryEmptyPosition) {
  for (std::size_t offset = 4; offset < 16; ++offset) {
    SCOPED_TRACE(offset);
    alignas(128) std::array<std::byte, 400> memory{};
    ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
    SpscQueue producer;
    SpscQueue consumer;
    ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
    ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
    std::array<std::byte, 12> payload{};
    ASSERT_EQ(producer.TryWrite(std::span(payload).first(offset - 4)),
              SpscStatus::kSuccess);
    ASSERT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
    // The coarse probe must allow the real reserve to publish padding.
    EXPECT_EQ(producer.CheckWrite(12), SpscStatus::kSuccess);
    auto blocked = producer.TryReserve(12);
    EXPECT_EQ(blocked.status, SpscStatus::kFull);
    EXPECT_FALSE(blocked.reservation.active());
    EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
    auto reservation = producer.TryReserve(12);
    ASSERT_EQ(reservation.status, SpscStatus::kSuccess);
    EXPECT_EQ(reservation.reservation.payload().data(), memory.data() + 388);
    std::fill(reservation.reservation.payload().begin(),
              reservation.reservation.payload().end(), std::byte{0x43});
    ASSERT_EQ(reservation.reservation.Commit(12), SpscStatus::kSuccess);
    EXPECT_EQ(producer.CheckWrite(0), SpscStatus::kFull);
    auto acquired = consumer.TryAcquire();
    ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
    EXPECT_EQ(acquired.lease.size(), 12U);
    ASSERT_EQ(acquired.lease.Release(), SpscStatus::kSuccess);
  }
}

TEST(SpscQueueWrapTest, ExactTailFitDoesNotPublishPadding) {
  alignas(128) std::array<std::byte, 400> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
  SpscQueue producer;
  SpscQueue consumer;
  ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
  std::array<std::byte, 8> payload{};
  ASSERT_EQ(producer.TryWrite(std::span(payload).first(4)),
            SpscStatus::kSuccess);
  ASSERT_EQ(producer.TryWrite(std::span(payload).first(4)),
            SpscStatus::kSuccess);
  EXPECT_EQ(producer.TryWrite({}), SpscStatus::kFull);
  EXPECT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
  EXPECT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
  EXPECT_EQ(consumer.TryRead(payload).status, SpscStatus::kEmpty);
}

TEST(SpscQueueWrapTest, CancelAndSmallOutputRetainPublishedPaddingSemantics) {
  alignas(128) std::array<std::byte, 400> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
  SpscQueue producer;
  SpscQueue consumer;
  ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
  std::array<std::byte, 8> payload;
  payload.fill(std::byte{0x72});
  ASSERT_EQ(producer.TryWrite(payload), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
  // Offset 12 leaves four bytes; reserve publishes padding before borrowing.
  auto reserved = producer.TryReserve(8);
  ASSERT_EQ(reserved.status, SpscStatus::kSuccess);
  reserved.reservation.Cancel();
  EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
  ASSERT_EQ(producer.TryWrite(payload), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
  ASSERT_EQ(producer.TryWrite(payload), SpscStatus::kSuccess);
  std::array<std::byte, 3> small;
  small.fill(std::byte{0xf1});
  auto read = consumer.TryRead(small);
  EXPECT_EQ(read.status, SpscStatus::kBufferTooSmall);
  EXPECT_EQ(read.payload_size, payload.size());
  EXPECT_EQ(small, (std::array<std::byte, 3>{std::byte{0xf1}, std::byte{0xf1},
                                             std::byte{0xf1}}));
  auto acquired = consumer.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                         acquired.lease.payload().begin()));
  acquired.lease.Cancel();
  EXPECT_EQ(consumer.TryRead(payload).status, SpscStatus::kSuccess);
}

class FixedRandom {
 public:
  explicit FixedRandom(std::uint64_t seed) : state_(seed) {}
  std::uint64_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 7;
    state_ ^= state_ << 17;
    return state_;
  }

 private:
  std::uint64_t state_;
};

TEST(SpscQueueModelTest, RandomOperationsPreserveReferenceFrameSequence) {
  using Reservation =
      decltype(std::declval<SpscQueue&>().TryReserve(0).reservation);
  using Lease = decltype(std::declval<SpscQueue&>().TryAcquire().lease);
  for (std::uint64_t capacity : {4U, 8U, 16U, 32U, 64U}) {
    for (std::uint64_t seed : {1U, 7U, 37U, 101U}) {
      SCOPED_TRACE(capacity);
      SCOPED_TRACE(seed);
      alignas(128) std::array<std::byte, 448> memory{};
      ASSERT_EQ(SpscQueue::Initialize(memory, capacity), SpscStatus::kSuccess);
      SpscQueue producer;
      SpscQueue consumer;
      ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
      ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
      FixedRandom random(seed);
      std::deque<std::vector<std::byte>> frames;
      Reservation reservation;
      Lease lease;
      std::vector<std::byte> pending;
      for (int step = 0; step < 12000; ++step) {
        const auto operation = random.Next() % 10;
        if (operation <= 1 && !reservation.active()) {
          const auto size = random.Next() % (capacity - 3);
          auto result = producer.TryReserve(size);
          if (result.status == SpscStatus::kSuccess) {
            ASSERT_TRUE(result.reservation.active());
            ASSERT_EQ(result.reservation.payload().size(), size);
            ASSERT_GE(result.reservation.payload().data(), memory.data() + 384);
            ASSERT_LE(result.reservation.payload().data() + size,
                      memory.data() + 384 + capacity);
            pending.resize(size);
            for (auto& value : pending) {
              value = static_cast<std::byte>(random.Next() & 0xff);
            }
            std::copy(pending.begin(), pending.end(),
                      result.reservation.payload().begin());
            reservation = std::move(result.reservation);
          } else {
            ASSERT_EQ(result.status, SpscStatus::kFull);
            EXPECT_FALSE(result.reservation.active());
          }
        } else if (operation == 2 && reservation.active()) {
          const auto size = random.Next() % (pending.size() + 1);
          ASSERT_EQ(reservation.Commit(size), SpscStatus::kSuccess);
          pending.resize(size);
          frames.push_back(pending);
        } else if (operation == 3 && reservation.active()) {
          if ((random.Next() & 1) == 0) {
            EXPECT_EQ(reservation.Commit(pending.size() + 1),
                      SpscStatus::kInvalidSize);
            EXPECT_TRUE(reservation.active());
          } else {
            reservation = Reservation{};
          }
        } else if (operation == 4 && !lease.active()) {
          auto result = consumer.TryAcquire();
          if (frames.empty()) {
            ASSERT_EQ(result.status, SpscStatus::kEmpty);
          } else {
            ASSERT_EQ(result.status, SpscStatus::kSuccess);
            ASSERT_EQ(result.lease.size(), frames.front().size());
            ASSERT_TRUE(std::equal(frames.front().begin(), frames.front().end(),
                                   result.lease.payload().begin()));
            lease = std::move(result.lease);
          }
        } else if (operation == 5 && lease.active()) {
          ASSERT_FALSE(frames.empty());
          ASSERT_TRUE(std::equal(frames.front().begin(), frames.front().end(),
                                 lease.payload().begin()));
          if ((random.Next() & 1) == 0) {
            ASSERT_EQ(lease.Release(), SpscStatus::kSuccess);
            frames.pop_front();
          } else {
            lease = Lease{};
          }
        } else if (operation == 6) {
          std::array<std::byte, 64> output;
          output.fill(std::byte{0xf8});
          const auto size = random.Next() % (capacity + 1);
          auto result = consumer.TryRead(std::span(output).first(size));
          if (lease.active()) {
            ASSERT_EQ(result.status, SpscStatus::kBusy);
          } else if (frames.empty()) {
            ASSERT_EQ(result.status, SpscStatus::kEmpty);
          } else if (size < frames.front().size()) {
            ASSERT_EQ(result.status, SpscStatus::kBufferTooSmall);
            EXPECT_EQ(result.payload_size, frames.front().size());
          } else {
            ASSERT_EQ(result.status, SpscStatus::kSuccess);
            ASSERT_EQ(result.payload_size, frames.front().size());
            ASSERT_TRUE(std::equal(frames.front().begin(), frames.front().end(),
                                   output.begin()));
            frames.pop_front();
          }
          if (result.status != SpscStatus::kSuccess) {
            ASSERT_TRUE(std::all_of(
                output.begin(), output.end(),
                [](std::byte value) { return value == std::byte{0xf8}; }));
          }
        } else if (operation == 7) {
          const auto size = random.Next() % (capacity - 3);
          std::vector<std::byte> payload(size);
          for (auto& value : payload) {
            value = static_cast<std::byte>(random.Next() & 0xff);
          }
          const auto status = producer.TryWrite(payload);
          if (reservation.active()) {
            ASSERT_EQ(status, SpscStatus::kBusy);
          } else if (status == SpscStatus::kSuccess) {
            frames.push_back(payload);
          } else {
            ASSERT_EQ(status, SpscStatus::kFull);
          }
        } else if (operation == 8) {
          if (reservation.active()) {
            EXPECT_EQ(producer.TryReserve(0).status, SpscStatus::kBusy);
          }
          if (lease.active()) {
            EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kBusy);
            ASSERT_TRUE(std::equal(frames.front().begin(), frames.front().end(),
                                   lease.payload().begin()));
          }
        } else if (operation == 9) {
          EXPECT_EQ(producer.CheckWrite(capacity), reservation.active()
                                                       ? SpscStatus::kBusy
                                                       : SpscStatus::kTooLarge);
        }
      }
      reservation.Cancel();
      if (lease.active()) {
        ASSERT_EQ(lease.Release(), SpscStatus::kSuccess);
        frames.pop_front();
      }
      for (const auto& expected : frames) {
        auto result = consumer.TryAcquire();
        ASSERT_EQ(result.status, SpscStatus::kSuccess);
        ASSERT_EQ(result.lease.size(), expected.size());
        ASSERT_TRUE(std::equal(expected.begin(), expected.end(),
                               result.lease.payload().begin()));
        ASSERT_EQ(result.lease.Release(), SpscStatus::kSuccess);
      }
      EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
    }
  }
}

}  // namespace
}  // namespace yquant::base
