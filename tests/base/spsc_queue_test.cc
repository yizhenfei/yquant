#include "yquant/base/spsc_queue.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include "gtest/gtest.h"

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;

static_assert(!std::is_copy_constructible_v<SpscQueue>);
static_assert(!std::is_move_constructible_v<SpscQueue>);
static_assert(std::is_nothrow_default_constructible_v<SpscQueue>);
static_assert(std::is_nothrow_destructible_v<SpscQueue>);

std::span<const std::byte> Bytes(const char* text) {
  return {reinterpret_cast<const std::byte*>(text), std::strlen(text)};
}

class SpscQueueTest : public ::testing::Test {
 protected:
  static constexpr std::uint64_t kCapacity = 64;

  void SetUp() override {
    ASSERT_EQ(SpscQueue::Initialize(memory_, kCapacity), SpscStatus::kSuccess);
    ASSERT_EQ(producer_.Attach(memory_, Role::kProducer), SpscStatus::kSuccess);
    ASSERT_EQ(consumer_.Attach(memory_, Role::kConsumer), SpscStatus::kSuccess);
  }

  void ExpectRead(std::span<const std::byte> expected) {
    std::array<std::byte, 128> output{};
    auto result = consumer_.TryRead(output);
    ASSERT_EQ(result.status, SpscStatus::kSuccess);
    ASSERT_EQ(result.payload_size, expected.size());
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), output.begin()));
  }

  alignas(128) std::array<std::byte, 512> memory_{};
  SpscQueue producer_;
  SpscQueue consumer_;
};

TEST(SpscQueueLayoutTest, ComputesRequiredBytesAndRejectsInvalidCapacity) {
  EXPECT_EQ(SpscQueue::required_alignment(), 128U);
  for (std::uint64_t capacity : {4U, 8U, 16U, 64U, 1024U}) {
    auto result = SpscQueue::RequiredBytes(capacity);
    EXPECT_EQ(result.status, SpscStatus::kSuccess);
    EXPECT_EQ(result.bytes, 384U + capacity);
  }
  const auto largest = SpscQueue::RequiredBytes(std::uint64_t{1} << 62);
  EXPECT_EQ(largest.status, SpscStatus::kSuccess);
  EXPECT_EQ(largest.bytes, (std::uint64_t{1} << 62) + 384);
  for (std::uint64_t capacity : std::array<std::uint64_t, 8>{
           0, 1, 2, 3, 5, 63, std::uint64_t{1} << 63,
           std::numeric_limits<std::uint64_t>::max()}) {
    auto result = SpscQueue::RequiredBytes(capacity);
    EXPECT_EQ(result.status, SpscStatus::kInvalidArgument);
    EXPECT_EQ(result.bytes, 0U);
  }
}

TEST(SpscQueueLayoutTest, FailedInitializationDoesNotModifyMemory) {
  alignas(128) std::array<std::byte, 512> memory;
  memory.fill(std::byte{0xa7});
  const auto original = memory;
  EXPECT_EQ(SpscQueue::Initialize(memory, 63), SpscStatus::kInvalidArgument);
  EXPECT_EQ(memory, original);
  EXPECT_EQ(SpscQueue::Initialize(std::span(memory).first(447), 64),
            SpscStatus::kInsufficientStorage);
  EXPECT_EQ(memory, original);
  EXPECT_EQ(SpscQueue::Initialize(std::span(memory).subspan(1), 64),
            SpscStatus::kMisalignedStorage);
  EXPECT_EQ(memory, original);
  EXPECT_EQ(SpscQueue::Initialize({}, 64), SpscStatus::kInvalidArgument);
}

TEST(SpscQueueLayoutTest, InitializesOnlyControlRegion) {
  alignas(128) std::array<std::byte, 512> memory;
  memory.fill(std::byte{0xc5});
  ASSERT_EQ(SpscQueue::Initialize(memory, 64), SpscStatus::kSuccess);
  EXPECT_TRUE(
      std::all_of(memory.begin() + 384, memory.end(),
                  [](std::byte value) { return value == std::byte{0xc5}; }));
  SpscQueue consumer;
  ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
  EXPECT_EQ(consumer.TryAcquire().status, SpscStatus::kEmpty);
}

TEST(SpscQueueLayoutTest, UnboundViewsAndWrongRolesRejectOperations) {
  SpscQueue queue;
  EXPECT_FALSE(queue.attached());
  EXPECT_EQ(queue.role(), Role::kUnbound);
  EXPECT_EQ(queue.capacity(), 0U);
  EXPECT_EQ(queue.max_payload_size(), 0U);
  EXPECT_EQ(queue.CheckWrite(0), SpscStatus::kInvalidState);
  EXPECT_EQ(queue.TryReserve(0).status, SpscStatus::kInvalidState);
  EXPECT_EQ(queue.TryWrite({}), SpscStatus::kInvalidState);
  EXPECT_EQ(queue.TryAcquire().status, SpscStatus::kInvalidState);
  std::array<std::byte, 4> output{};
  EXPECT_EQ(queue.TryRead(output).status, SpscStatus::kInvalidState);
  EXPECT_EQ(queue.Detach(), SpscStatus::kSuccess);
  EXPECT_EQ(queue.Detach(), SpscStatus::kSuccess);
}

TEST_F(SpscQueueTest, BindingValidatesStorageAndPreservesLocalState) {
  SpscQueue view;
  EXPECT_EQ(view.Attach(memory_, Role::kUnbound), SpscStatus::kInvalidArgument);
  EXPECT_EQ(view.Attach({}, Role::kProducer), SpscStatus::kInvalidArgument);
  EXPECT_EQ(view.Attach(std::span(memory_).subspan(1), Role::kProducer),
            SpscStatus::kMisalignedStorage);
  EXPECT_EQ(view.Attach(std::span(memory_).first(383), Role::kProducer),
            SpscStatus::kInsufficientStorage);
  EXPECT_FALSE(view.attached());
  ASSERT_EQ(view.Attach(memory_, Role::kProducer), SpscStatus::kSuccess);
  EXPECT_EQ(view.Attach(memory_, Role::kConsumer), SpscStatus::kInvalidState);
  EXPECT_EQ(view.role(), Role::kProducer);
  EXPECT_EQ(view.capacity(), kCapacity);
  EXPECT_EQ(view.max_payload_size(), kCapacity - 4);
  EXPECT_EQ(view.TryAcquire().status, SpscStatus::kInvalidState);
  EXPECT_EQ(view.TryRead({}).status, SpscStatus::kInvalidState);
  EXPECT_EQ(consumer_.TryReserve(0).status, SpscStatus::kInvalidState);
  EXPECT_EQ(consumer_.TryWrite({}), SpscStatus::kInvalidState);
  EXPECT_EQ(consumer_.CheckWrite(0), SpscStatus::kInvalidState);
  ASSERT_EQ(view.Detach(), SpscStatus::kSuccess);
  EXPECT_FALSE(view.attached());
  EXPECT_EQ(view.role(), Role::kUnbound);
  EXPECT_EQ(view.capacity(), 0U);
  ASSERT_EQ(view.Attach(memory_, Role::kConsumer), SpscStatus::kSuccess);
}

TEST_F(SpscQueueTest, CopiesOneFrameAtATimeAndPreservesOutputTail) {
  ASSERT_EQ(producer_.TryWrite(Bytes("first")), SpscStatus::kSuccess);
  ASSERT_EQ(producer_.TryWrite(Bytes("second")), SpscStatus::kSuccess);
  std::array<std::byte, 128> output;
  output.fill(std::byte{0xe2});
  auto result = consumer_.TryRead(output);
  ASSERT_EQ(result.status, SpscStatus::kSuccess);
  EXPECT_EQ(result.payload_size, 5U);
  EXPECT_EQ(std::memcmp(output.data(), "first", 5), 0);
  EXPECT_TRUE(
      std::all_of(output.begin() + 5, output.end(),
                  [](std::byte value) { return value == std::byte{0xe2}; }));
  ExpectRead(Bytes("second"));
  output.fill(std::byte{0xe2});
  result = consumer_.TryRead(output);
  EXPECT_EQ(result.status, SpscStatus::kEmpty);
  EXPECT_EQ(result.payload_size, 0U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](std::byte value) {
    return value == std::byte{0xe2};
  }));
}

TEST_F(SpscQueueTest, EmptyFrameIsAnActiveBorrowAndNotAnEmptyQueue) {
  auto reserved = producer_.TryReserve(0);
  ASSERT_EQ(reserved.status, SpscStatus::kSuccess);
  EXPECT_TRUE(reserved.reservation.active());
  EXPECT_TRUE(reserved.reservation.payload().empty());
  EXPECT_EQ(consumer_.TryAcquire().status, SpscStatus::kEmpty);
  ASSERT_EQ(reserved.reservation.Commit(0), SpscStatus::kSuccess);
  auto acquired = consumer_.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  EXPECT_TRUE(acquired.lease.active());
  EXPECT_TRUE(acquired.lease.payload().empty());
  EXPECT_EQ(acquired.lease.size(), 0U);
  ASSERT_EQ(acquired.lease.Release(), SpscStatus::kSuccess);
  EXPECT_EQ(acquired.lease.Release(), SpscStatus::kInvalidState);
  ASSERT_EQ(producer_.TryWrite({}), SpscStatus::kSuccess);
  auto result = consumer_.TryRead({});
  EXPECT_EQ(result.status, SpscStatus::kSuccess);
  EXPECT_EQ(result.payload_size, 0U);
  EXPECT_EQ(consumer_.TryRead({}).status, SpscStatus::kEmpty);
}

TEST_F(SpscQueueTest, UncommittedWriteIsInvisibleAndShortCommitKeepsPrefix) {
  auto result = producer_.TryReserve(60);
  ASSERT_EQ(result.status, SpscStatus::kSuccess);
  auto& reservation = result.reservation;
  EXPECT_EQ(reservation.payload().size(), 60U);
  std::fill(reservation.payload().begin(), reservation.payload().end(),
            std::byte{0x71});
  EXPECT_EQ(consumer_.TryAcquire().status, SpscStatus::kEmpty);
  EXPECT_EQ(reservation.Commit(61), SpscStatus::kInvalidSize);
  EXPECT_TRUE(reservation.active());
  EXPECT_EQ(producer_.TryReserve(0).status, SpscStatus::kBusy);
  EXPECT_EQ(producer_.CheckWrite(0), SpscStatus::kBusy);
  EXPECT_EQ(producer_.TryWrite({}), SpscStatus::kBusy);
  EXPECT_EQ(producer_.TryAcquire().status, SpscStatus::kInvalidState);
  EXPECT_EQ(producer_.Detach(), SpscStatus::kBusy);
  auto* original_payload = reservation.payload().data();
  ASSERT_EQ(reservation.Commit(3), SpscStatus::kSuccess);
  EXPECT_FALSE(reservation.active());
  EXPECT_TRUE(reservation.payload().empty());
  EXPECT_EQ(reservation.Commit(0), SpscStatus::kInvalidState);
  auto acquired = consumer_.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  EXPECT_EQ(acquired.lease.payload().data(), original_payload);
  EXPECT_EQ(acquired.lease.size(), 3U);
  EXPECT_TRUE(std::all_of(
      acquired.lease.payload().begin(), acquired.lease.payload().end(),
      [](std::byte value) { return value == std::byte{0x71}; }));
  // The unused suffix of a short reservation is immediately reusable.
  auto next = producer_.TryReserve(53);
  ASSERT_EQ(next.status, SpscStatus::kSuccess);
  next.reservation.Cancel();
  ASSERT_EQ(acquired.lease.Release(), SpscStatus::kSuccess);
}

TEST_F(SpscQueueTest, CancellationAndDestructionDoNotPublishOrConsumeFrames) {
  {
    auto result = producer_.TryReserve(10);
    ASSERT_EQ(result.status, SpscStatus::kSuccess);
    result.reservation.payload()[0] = std::byte{0x12};
  }
  EXPECT_EQ(consumer_.TryAcquire().status, SpscStatus::kEmpty);
  auto reserved = producer_.TryReserve(4);
  ASSERT_EQ(reserved.status, SpscStatus::kSuccess);
  reserved.reservation.Cancel();
  reserved.reservation.Cancel();
  EXPECT_EQ(reserved.reservation.Commit(0), SpscStatus::kInvalidState);
  ASSERT_EQ(producer_.TryWrite(Bytes("retained")), SpscStatus::kSuccess);
  {
    auto acquired = consumer_.TryAcquire();
    ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  }
  auto acquired = consumer_.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  acquired.lease.Cancel();
  acquired.lease.Cancel();
  EXPECT_EQ(acquired.lease.Release(), SpscStatus::kInvalidState);
  EXPECT_EQ(acquired.lease.size(), 0U);
  EXPECT_TRUE(acquired.lease.payload().empty());
  ExpectRead(Bytes("retained"));
}

TEST_F(SpscQueueTest, PinnedReadBlocksReuseButAllowsOtherEndToContinue) {
  std::array<std::byte, 28> payload;
  payload.fill(std::byte{0x3c});
  ASSERT_EQ(producer_.TryWrite(payload), SpscStatus::kSuccess);
  auto acquired = consumer_.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  EXPECT_EQ(consumer_.TryAcquire().status, SpscStatus::kBusy);
  EXPECT_EQ(consumer_.TryRead({}).status, SpscStatus::kBusy);
  EXPECT_EQ(consumer_.Detach(), SpscStatus::kBusy);
  ASSERT_EQ(producer_.TryWrite(payload), SpscStatus::kSuccess);
  EXPECT_EQ(producer_.TryReserve(0).status, SpscStatus::kFull);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                         acquired.lease.payload().begin()));
  acquired.lease.Cancel();
  ExpectRead(payload);
  ExpectRead(payload);
}

TEST_F(SpscQueueTest, InsufficientOutputRetainsFrameAndDoesNotWriteOutput) {
  ASSERT_EQ(producer_.TryWrite(Bytes("larger")), SpscStatus::kSuccess);
  std::array<std::byte, 5> output;
  output.fill(std::byte{0xa3});
  auto result = consumer_.TryRead(output);
  EXPECT_EQ(result.status, SpscStatus::kBufferTooSmall);
  EXPECT_EQ(result.payload_size, 6U);
  EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](std::byte value) {
    return value == std::byte{0xa3};
  }));
  ExpectRead(Bytes("larger"));
}

TEST_F(SpscQueueTest, RejectsPayloadBeyondLengthLimitWithoutBorrow) {
  EXPECT_EQ(producer_.CheckWrite(61), SpscStatus::kTooLarge);
  auto result = producer_.TryReserve(61);
  EXPECT_EQ(result.status, SpscStatus::kTooLarge);
  EXPECT_FALSE(result.reservation.active());
  EXPECT_EQ(
      producer_.TryReserve(std::numeric_limits<std::size_t>::max()).status,
      SpscStatus::kTooLarge);
  ASSERT_EQ(producer_.TryWrite({}), SpscStatus::kSuccess);
  EXPECT_EQ(consumer_.TryRead({}).status, SpscStatus::kSuccess);
}

TEST_F(SpscQueueTest, BorrowMovesTransferOwnershipAndCancelAssignmentTarget) {
  auto first = producer_.TryReserve(8);
  ASSERT_EQ(first.status, SpscStatus::kSuccess);
  using Reservation = decltype(first.reservation);
  static_assert(!std::is_copy_constructible_v<Reservation>);
  static_assert(std::is_nothrow_move_constructible_v<Reservation>);
  static_assert(std::is_nothrow_move_assignable_v<Reservation>);
  Reservation moved(std::move(first.reservation));
  EXPECT_FALSE(first.reservation.active());
  EXPECT_TRUE(moved.active());
  Reservation empty;
  empty = std::move(moved);
  EXPECT_FALSE(moved.active());
  EXPECT_TRUE(empty.active());
  Reservation* same = &empty;
  empty = std::move(*same);
  EXPECT_TRUE(empty.active());
  // Assigning an inactive handle cancels the target's pending reservation.
  empty = Reservation{};
  EXPECT_FALSE(empty.active());
  ASSERT_EQ(producer_.TryWrite(Bytes("move")), SpscStatus::kSuccess);
  auto acquired = consumer_.TryAcquire();
  ASSERT_EQ(acquired.status, SpscStatus::kSuccess);
  using Lease = decltype(acquired.lease);
  static_assert(!std::is_copy_constructible_v<Lease>);
  static_assert(std::is_nothrow_move_constructible_v<Lease>);
  auto* payload = acquired.lease.payload().data();
  Lease lease(std::move(acquired.lease));
  EXPECT_FALSE(acquired.lease.active());
  EXPECT_EQ(lease.payload().data(), payload);
  Lease destination;
  destination = std::move(lease);
  EXPECT_FALSE(lease.active());
  Lease* same_lease = &destination;
  destination = std::move(*same_lease);
  EXPECT_TRUE(destination.active());
  destination = Lease{};
  ExpectRead(Bytes("move"));
}

TEST_F(SpscQueueTest, MoveAssignmentCancelsAnotherQueuesActiveBorrow) {
  alignas(128) std::array<std::byte, 448> other_memory{};
  ASSERT_EQ(SpscQueue::Initialize(other_memory, 64), SpscStatus::kSuccess);
  SpscQueue other_producer;
  SpscQueue other_consumer;
  ASSERT_EQ(other_producer.Attach(other_memory, Role::kProducer),
            SpscStatus::kSuccess);
  ASSERT_EQ(other_consumer.Attach(other_memory, Role::kConsumer),
            SpscStatus::kSuccess);
  auto first = producer_.TryReserve(4);
  auto second = other_producer.TryReserve(8);
  ASSERT_EQ(first.status, SpscStatus::kSuccess);
  ASSERT_EQ(second.status, SpscStatus::kSuccess);
  auto* payload = first.reservation.payload().data();
  second.reservation = std::move(first.reservation);
  EXPECT_FALSE(first.reservation.active());
  EXPECT_EQ(second.reservation.payload().data(), payload);
  EXPECT_EQ(producer_.TryWrite({}), SpscStatus::kBusy);
  EXPECT_EQ(other_producer.TryWrite({}), SpscStatus::kSuccess);
  second.reservation.Cancel();
  EXPECT_EQ(producer_.TryWrite({}), SpscStatus::kSuccess);
  auto first_read = consumer_.TryAcquire();
  auto second_read = other_consumer.TryAcquire();
  ASSERT_EQ(first_read.status, SpscStatus::kSuccess);
  ASSERT_EQ(second_read.status, SpscStatus::kSuccess);
  second_read.lease = std::move(first_read.lease);
  EXPECT_FALSE(first_read.lease.active());
  EXPECT_EQ(consumer_.TryAcquire().status, SpscStatus::kBusy);
  // The target's old frame was unpinned without being consumed.
  EXPECT_EQ(other_consumer.TryRead({}).status, SpscStatus::kSuccess);
  ASSERT_EQ(second_read.lease.Release(), SpscStatus::kSuccess);
}

TEST(SpscQueueHandleTest, DefaultHandlesAreInactiveAndSafeToCancel) {
  SpscQueue::WriteReservation reservation;
  SpscQueue::ReadLease lease;
  EXPECT_FALSE(reservation.active());
  EXPECT_TRUE(reservation.payload().empty());
  EXPECT_EQ(reservation.Commit(0), SpscStatus::kInvalidState);
  reservation.Cancel();
  EXPECT_FALSE(lease.active());
  EXPECT_TRUE(lease.payload().empty());
  EXPECT_EQ(lease.size(), 0U);
  EXPECT_EQ(lease.Release(), SpscStatus::kInvalidState);
  lease.Cancel();
}

TEST(SpscQueueSmallCapacityTest, HoldsExactlyOneEmptyFrame) {
  alignas(128) std::array<std::byte, 388> memory{};
  ASSERT_EQ(SpscQueue::Initialize(memory, 4), SpscStatus::kSuccess);
  SpscQueue producer;
  SpscQueue consumer;
  ASSERT_EQ(producer.Attach(memory, Role::kProducer), SpscStatus::kSuccess);
  ASSERT_EQ(consumer.Attach(memory, Role::kConsumer), SpscStatus::kSuccess);
  EXPECT_EQ(producer.max_payload_size(), 0U);
  ASSERT_EQ(producer.TryWrite({}), SpscStatus::kSuccess);
  EXPECT_EQ(producer.TryWrite({}), SpscStatus::kFull);
  EXPECT_EQ(producer.TryReserve(1).status, SpscStatus::kTooLarge);
  EXPECT_EQ(consumer.TryRead({}).status, SpscStatus::kSuccess);
  EXPECT_EQ(consumer.TryRead({}).status, SpscStatus::kEmpty);
  EXPECT_EQ(producer.TryWrite({}), SpscStatus::kSuccess);
}

TEST_F(SpscQueueTest, DetachAndReattachDoNotResetSharedState) {
  ASSERT_EQ(producer_.TryWrite(Bytes("persist")), SpscStatus::kSuccess);
  ASSERT_EQ(producer_.Detach(), SpscStatus::kSuccess);
  ASSERT_EQ(consumer_.Detach(), SpscStatus::kSuccess);
  ASSERT_EQ(producer_.Attach(memory_, Role::kProducer), SpscStatus::kSuccess);
  ASSERT_EQ(consumer_.Attach(memory_, Role::kConsumer), SpscStatus::kSuccess);
  ExpectRead(Bytes("persist"));
}

TEST(SpscQueueLifecycleDeathTest, DestroyingViewWithBorrowTerminates) {
  EXPECT_DEATH(
      ([] {
        alignas(128) std::array<std::byte, 448> memory{};
        if (SpscQueue::Initialize(memory, 64) != SpscStatus::kSuccess) {
          std::abort();
        }
        auto* view = new SpscQueue;
        if (view->Attach(memory, Role::kProducer) != SpscStatus::kSuccess) {
          std::abort();
        }
        auto result = view->TryReserve(0);
        delete view;
      }()),
      "");
}

}  // namespace
}  // namespace yquant::base
