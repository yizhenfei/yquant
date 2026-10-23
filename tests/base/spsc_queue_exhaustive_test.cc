#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>

#include "base/internal/spsc_queue_layout.h"
#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace yquant::base {
namespace {

// Return fixed diagnostic text so successful cases do not create a GoogleTest
// trace or message for each combination in the exhaustive loop.
const char* CheckEmptyQueueBoundary(uint64_t capacity, uint64_t position,
                                    size_t reserved_size,
                                    size_t committed_size) {
  constexpr std::byte kSentinel{0x66};
  alignas(128) std::array<std::byte, 640> memory;
  memory.fill(kSentinel);
  if (SpscQueue::Initialize(memory, capacity) != SpscStatus::kSuccess) {
    return "initialize";
  }
  auto* control = reinterpret_cast<internal::SpscControlBlock*>(memory.data());
  // No endpoint has attached yet. Setting both positions establishes an empty
  // queue at an arbitrary physical offset without billions of warm-up frames.
  control->write.position.store(position);
  control->read.position.store(position);
  SpscQueue producer;
  SpscQueue consumer;
  if (producer.Attach(memory, SpscQueue::Role::kProducer) !=
          SpscStatus::kSuccess ||
      consumer.Attach(memory, SpscQueue::Role::kConsumer) !=
          SpscStatus::kSuccess) {
    return "attach";
  }
  if (producer.CheckWrite(reserved_size) != SpscStatus::kSuccess) {
    return "coarse empty-queue capacity check";
  }
  auto result = producer.TryReserve(reserved_size);
  if (result.status == SpscStatus::kFull) {
    if (consumer.TryAcquire().status != SpscStatus::kEmpty) {
      return "reclaim padding-only publication";
    }
    result = producer.TryReserve(reserved_size);
  }
  if (result.status != SpscStatus::kSuccess) {
    return "reserve after tail reclaim";
  }
  auto output = result.reservation.payload();
  const auto data_begin =
      reinterpret_cast<uintptr_t>(memory.data()) + internal::kSpscDataOffset;
  const auto payload_begin = reinterpret_cast<uintptr_t>(output.data());
  if (output.size() != reserved_size ||
      payload_begin < data_begin + internal::kSpscFrameHeaderBytes ||
      payload_begin > data_begin + capacity ||
      output.size() > data_begin + capacity - payload_begin) {
    return "contiguous frame bounds";
  }
  for (size_t i = 0; i < output.size(); ++i) {
    output[i] = static_cast<std::byte>(i ^ 0xa5);
  }
  const std::byte* payload_address = output.data();
  // This can reclaim published padding while the writer still owns its data
  // reservation; the reservation must remain valid and unpublished.
  if (consumer.TryAcquire().status != SpscStatus::kEmpty) {
    return "uncommitted frame visibility";
  }
  if (result.reservation.Commit(committed_size) != SpscStatus::kSuccess) {
    return "short commit";
  }
  auto read = consumer.TryAcquire();
  if (read.status != SpscStatus::kSuccess ||
      read.lease.size() != committed_size ||
      read.lease.payload().data() != payload_address) {
    return "committed prefix and zero-copy address";
  }
  for (size_t i = 0; i < committed_size; ++i) {
    if (read.lease.payload()[i] != static_cast<std::byte>(i ^ 0xa5)) {
      return "committed payload bytes";
    }
  }
  if (read.lease.Release() != SpscStatus::kSuccess ||
      consumer.TryAcquire().status != SpscStatus::kEmpty ||
      control->write.position.load() != control->read.position.load()) {
    return "drained positions";
  }
  if (!std::all_of(memory.begin() + internal::kSpscDataOffset + capacity,
                   memory.end(),
                   [](std::byte value) { return value == kSentinel; })) {
    return "storage beyond data capacity was modified";
  }
  return nullptr;
}

TEST(SpscQueueExhaustiveTest, EmptyPositionsLengthsAndCounterWrap) {
  uint64_t cases = 0;
  for (uint64_t capacity = 4; capacity <= 256; capacity *= 2) {
    for (bool near_counter_wrap : {false, true}) {
      const auto base =
          near_counter_wrap
              ? std::numeric_limits<uint64_t>::max() & ~(capacity - 1)
              : 0;
      for (uint64_t offset = 0; offset < capacity; ++offset) {
        for (size_t reserved = 0; reserved <= capacity - 4; ++reserved) {
          for (size_t committed : {size_t{0}, reserved / 2, reserved}) {
            const char* failure = CheckEmptyQueueBoundary(
                capacity, base + offset, reserved, committed);
            ASSERT_EQ(failure, nullptr)
                << "stage=" << (failure == nullptr ? "" : failure)
                << " capacity=" << capacity << " offset=" << offset
                << " reserved=" << reserved << " committed=" << committed
                << " near_counter_wrap=" << near_counter_wrap;
            ++cases;
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 515112U);
}

}  // namespace
}  // namespace yquant::base
