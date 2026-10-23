#include "base/internal/spsc_queue_layout.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <span>

#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;
using internal::SpscControlBlock;

void SetHeader(std::byte* destination, uint32_t value) {
  for (unsigned int index = 0; index < 4; ++index) {
    destination[index] = static_cast<std::byte>((value >> (8 * index)) & 0xff);
  }
}

TEST(SpscQueueMetadataTest, RejectsEveryIncompatibleMetadataField) {
  for (int field = 0; field < 4; ++field) {
    SCOPED_TRACE(field);
    alignas(128) std::array<std::byte, 448> memory{};
    ASSERT_EQ(SpscQueue::Initialize(memory, 64), SpscStatus::kSuccess);
    auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
    switch (field) {
      case 0:
        ++control->metadata.magic;
        break;
      case 1:
        ++control->metadata.version;
        break;
      case 2:
        ++control->metadata.data_offset;
        break;
      case 3:
        control->metadata.capacity = 63;
        break;
    }
    SpscQueue view;
    EXPECT_EQ(view.Attach(memory, Role::kProducer),
              SpscStatus::kIncompatibleLayout);
    EXPECT_FALSE(view.attached());
    EXPECT_EQ(view.capacity(), 0U);
  }
}

TEST(SpscQueueMetadataTest, RejectsValidLayoutInIncompleteStorage) {
  alignas(128) std::array<std::byte, 448> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 64), SpscStatus::kSuccess);
  SpscQueue view;
  EXPECT_EQ(view.Attach(std::span(memory).first(447), Role::kProducer),
            SpscStatus::kInsufficientStorage);
  EXPECT_FALSE(view.attached());
}

TEST(SpscQueueCorruptionTest, RejectsImpossibleOccupancyOnBothEndpoints) {
  alignas(128) std::array<std::byte, 400> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
  SpscQueue producer;
  SpscQueue consumer;
  ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
  auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
  control->read.position.store(7, std::memory_order_relaxed);
  control->write.position.store(24, std::memory_order_relaxed);
  EXPECT_EQ(producer.CheckWrite(0), SpscStatus::kCorrupt);
  auto reserved = producer.TryReserve(0);
  EXPECT_EQ(reserved.status, SpscStatus::kCorrupt);
  EXPECT_FALSE(reserved.reservation.active());
  auto acquired = consumer.TryAcquire();
  EXPECT_EQ(acquired.status, SpscStatus::kCorrupt);
  EXPECT_FALSE(acquired.lease.active());
  std::array<std::byte, 16> output;
  output.fill(std::byte{0xaa});
  auto read = consumer.TryRead(output);
  EXPECT_EQ(read.status, SpscStatus::kCorrupt);
  EXPECT_EQ(read.payload_size, 0U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](std::byte value) {
    return value == std::byte{0xaa};
  }));
  EXPECT_EQ(control->read.position.load(), 7U);
  EXPECT_EQ(control->write.position.load(), 24U);
}

TEST(SpscQueueCounterTest, CoarseWriteCheckDoesNotPublishPaddingOrReserve) {
  alignas(128) std::array<std::byte, 400> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
  auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
  control->read.position.store(12, std::memory_order_relaxed);
  control->write.position.store(12, std::memory_order_relaxed);
  SpscQueue producer;
  ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
  EXPECT_EQ(producer.CheckWrite(12), SpscStatus::kSuccess);
  EXPECT_EQ(producer.CheckWrite(12), SpscStatus::kSuccess);
  EXPECT_EQ(control->read.position.load(), 12U);
  EXPECT_EQ(control->write.position.load(), 12U);
  EXPECT_EQ(producer.TryReserve(12).status, SpscStatus::kFull);
  EXPECT_EQ(control->read.position.load(), 12U);
  EXPECT_EQ(control->write.position.load(), 16U);
}

TEST(SpscQueueCorruptionTest, RejectsMalformedFramesWithoutConsumingThem) {
  struct Case {
    uint64_t read;
    uint64_t write;
    uint32_t header;
  };
  constexpr std::array<Case, 7> kCases{{
      {0, 1, 0},             // Incomplete frame header.
      {0, 4, 0xffffffffU},   // A wrap marker cannot occur at the ring head.
      {0, 16, 13},           // Payload exceeds the queue's length limit.
      {0, 8, 8},             // Frame extends beyond published bytes.
      {12, 20, 4},           // Frame would straddle the physical tail.
      {13, 15, 0},           // Only part of an implicit tail is published.
      {8, 12, 0xffffffffU},  // Only part of an explicit tail is published.
  }};
  for (const auto& test : kCases) {
    SCOPED_TRACE(test.read);
    SCOPED_TRACE(test.write);
    SCOPED_TRACE(test.header);
    alignas(128) std::array<std::byte, 400> memory{};
    ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
    SpscQueue consumer;
    ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
    auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
    control->read.position.store(test.read, std::memory_order_relaxed);
    control->write.position.store(test.write, std::memory_order_relaxed);
    if ((test.read & 15) <= 12) {
      SetHeader(memory.data() + 384 + (test.read & 15), test.header);
    }
    auto result = consumer.TryAcquire();
    EXPECT_EQ(result.status, SpscStatus::kCorrupt);
    EXPECT_FALSE(result.lease.active());
    EXPECT_EQ(control->read.position.load(), test.read);
    EXPECT_EQ(control->write.position.load(), test.write);
  }
}

TEST(SpscQueueCorruptionTest,
     PreservesReclaimedPaddingOnFollowingCorruptFrame) {
  for (uint64_t published_after_padding : {2U, 4U}) {
    SCOPED_TRACE(published_after_padding);
    alignas(128) std::array<std::byte, 400> memory{};
    ASSERT_EQ(SpscQueue::Initialize(memory, 16), SpscStatus::kSuccess);
    SpscQueue consumer;
    ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
    auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
    control->read.position.store(12, std::memory_order_relaxed);
    control->write.position.store(16 + published_after_padding,
                                  std::memory_order_relaxed);
    SetHeader(memory.data() + 396, 0xffffffffU);
    SetHeader(memory.data() + 384, 0xffffffffU);
    EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kCorrupt);
    EXPECT_EQ(control->read.position.load(), 16U);
  }
}

TEST(SpscQueueCounterTest, FramesAndPaddingWorkAcrossUint64Wrap) {
  constexpr auto kMax = std::numeric_limits<uint64_t>::max();
  for (uint64_t capacity : {4U, 8U, 16U, 32U}) {
    for (uint64_t offset = 0; offset < capacity; ++offset) {
      for (size_t size = 0; size <= capacity - 4; ++size) {
        SCOPED_TRACE(capacity);
        SCOPED_TRACE(offset);
        SCOPED_TRACE(size);
        alignas(128) std::array<std::byte, 416> memory{};
        ASSERT_EQ(SpscQueue::Initialize(memory, capacity),
                  SpscStatus::kSuccess);
        auto* control = reinterpret_cast<SpscControlBlock*>(memory.data());
        const auto start = kMax - capacity + 1 + offset;
        control->write.position.store(start, std::memory_order_relaxed);
        control->read.position.store(start, std::memory_order_relaxed);
        SpscQueue producer;
        SpscQueue consumer;
        ASSERT_EQ(producer.Attach(memory, Role::kProducer),
                  SpscStatus::kSuccess);
        ASSERT_EQ(consumer.Attach(memory, Role::kConsumer),
                  SpscStatus::kSuccess);
        EXPECT_EQ(producer.CheckWrite(size), SpscStatus::kSuccess);
        auto result = producer.TryReserve(size);
        const auto tail = capacity - offset;
        auto frame_start = start;
        if (4 + size > tail) {
          frame_start += tail;
          if (tail + 4 + size > capacity) {
            ASSERT_EQ(result.status, SpscStatus::kFull);
            EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
            result = producer.TryReserve(size);
          }
        }
        ASSERT_EQ(result.status, SpscStatus::kSuccess);
        for (size_t index = 0; index < size; ++index) {
          result.reservation.payload()[index] = static_cast<std::byte>(index);
        }
        EXPECT_GE(result.reservation.payload().data(), memory.data() + 384);
        EXPECT_LE(result.reservation.payload().data() + size,
                  memory.data() + 384 + capacity);
        ASSERT_EQ(result.reservation.Commit(size), SpscStatus::kSuccess);
        EXPECT_EQ(control->write.position.load(), frame_start + 4 + size);
        auto acquired = consumer.TryAcquire();
        ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
        EXPECT_EQ(acquired.lease.size(), size);
        for (size_t index = 0; index < size; ++index) {
          EXPECT_EQ(acquired.lease.payload()[index],
                    static_cast<std::byte>(index));
        }
        ASSERT_EQ(acquired.lease.Release(), SpscStatus::kSuccess);
        EXPECT_EQ(control->read.position.load(), frame_start + 4 + size);
        EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
      }
    }
  }
}

}  // namespace
}  // namespace yquant::base
