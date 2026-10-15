#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <utility>

#include "gtest/gtest.h"
#include "yquant/base/spsc_queue.h"

namespace {

struct CppHeapActivity {
  std::size_t allocation_calls = 0;
  std::size_t deallocation_calls = 0;
};

thread_local bool g_track_cpp_heap = false;
thread_local CppHeapActivity g_cpp_heap_activity;

// This executable counts replacement C++ allocation/deallocation functions.
// Direct calls to malloc, realloc, free, or OS mapping APIs are not counted.
void* AllocateCppHeap(std::size_t size, std::size_t alignment,
                      bool abort_on_failure) noexcept {
  if (g_track_cpp_heap) ++g_cpp_heap_activity.allocation_calls;
  size = std::max<std::size_t>(size, 1);
  void* result = nullptr;
  if (alignment == 0) {
    result = std::malloc(size);
  } else if (posix_memalign(&result, std::max(alignment, alignof(void*)),
                            size) != 0) {
    result = nullptr;
  }
  if (result == nullptr && abort_on_failure) std::abort();
  return result;
}

void DeallocateCppHeap(void* memory) noexcept {
  if (g_track_cpp_heap) ++g_cpp_heap_activity.deallocation_calls;
  std::free(memory);
}

template <typename Action>
CppHeapActivity ObserveCppHeap(Action&& action) noexcept {
  g_cpp_heap_activity = {};
  g_track_cpp_heap = true;
  std::forward<Action>(action)();
  g_track_cpp_heap = false;
  return g_cpp_heap_activity;
}

}  // namespace

void* operator new(std::size_t size) { return AllocateCppHeap(size, 0, true); }

void* operator new[](std::size_t size) {
  return AllocateCppHeap(size, 0, true);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, 0, false);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, 0, false);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  return AllocateCppHeap(size, static_cast<std::size_t>(alignment), true);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
  return AllocateCppHeap(size, static_cast<std::size_t>(alignment), true);
}

void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, static_cast<std::size_t>(alignment), false);
}

void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, static_cast<std::size_t>(alignment), false);
}

void operator delete(void* memory) noexcept { DeallocateCppHeap(memory); }

void operator delete[](void* memory) noexcept { DeallocateCppHeap(memory); }

void operator delete(void* memory, std::size_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete(void* memory, std::align_val_t,
                     const std::nothrow_t&) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, std::align_val_t,
                       const std::nothrow_t&) noexcept {
  DeallocateCppHeap(memory);
}

namespace yquant::base {
namespace {

using Role = SpscQueue::Role;

class SpscQueueCppHeapTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(SpscQueue::Initialize(memory_, 64), SpscStatus::kSuccess);
    ASSERT_EQ(SpscQueue::Initialize(other_memory_, 64), SpscStatus::kSuccess);
    ASSERT_EQ(producer_.Attach(memory_, Role::kProducer), SpscStatus::kSuccess);
    ASSERT_EQ(consumer_.Attach(memory_, Role::kConsumer), SpscStatus::kSuccess);
    ASSERT_EQ(other_producer_.Attach(other_memory_, Role::kProducer),
              SpscStatus::kSuccess);
    ASSERT_EQ(other_consumer_.Attach(other_memory_, Role::kConsumer),
              SpscStatus::kSuccess);
  }

  alignas(128) std::array<std::byte, 448> memory_{};
  alignas(128) std::array<std::byte, 448> other_memory_{};
  SpscQueue producer_;
  SpscQueue consumer_;
  SpscQueue other_producer_;
  SpscQueue other_consumer_;
};

TEST(SpscQueueCppHeapCounterTest, CountsReplacementOperatorCalls) {
  std::array<void*, 12> pointers{};
  std::array<bool, 12> allocated{};
  const auto activity = ObserveCppHeap([&]() noexcept {
    pointers[0] = ::operator new(16);
    pointers[1] = ::operator new[](16);
    pointers[2] = ::operator new(16, std::nothrow);
    pointers[3] = ::operator new[](16, std::nothrow);
    pointers[4] = ::operator new(16, std::align_val_t{128});
    pointers[5] = ::operator new[](16, std::align_val_t{128});
    pointers[6] = ::operator new(16, std::align_val_t{128}, std::nothrow);
    pointers[7] = ::operator new[](16, std::align_val_t{128}, std::nothrow);
    pointers[8] = ::operator new(16);
    pointers[9] = ::operator new[](16, std::align_val_t{128});
    pointers[10] = ::operator new[](16);
    pointers[11] = ::operator new(16, std::align_val_t{128});
    for (std::size_t i = 0; i < pointers.size(); ++i) {
      allocated[i] = pointers[i] != nullptr;
    }
    ::operator delete(pointers[0]);
    ::operator delete[](pointers[1], std::size_t{16});
    ::operator delete(pointers[2], std::nothrow);
    ::operator delete[](pointers[3], std::nothrow);
    ::operator delete(pointers[4], std::align_val_t{128});
    ::operator delete[](pointers[5], std::align_val_t{128});
    ::operator delete(pointers[6], std::align_val_t{128}, std::nothrow);
    ::operator delete[](pointers[7], std::align_val_t{128}, std::nothrow);
    ::operator delete(pointers[8], std::size_t{16});
    ::operator delete[](pointers[9], std::size_t{16}, std::align_val_t{128});
    ::operator delete[](pointers[10]);
    ::operator delete(pointers[11], std::size_t{16}, std::align_val_t{128});
  });
  EXPECT_EQ(activity.allocation_calls, 12U);
  EXPECT_EQ(activity.deallocation_calls, 12U);
  for (bool succeeded : allocated) EXPECT_TRUE(succeeded);
}

TEST(SpscQueueCppHeapCounterTest, ColdInitializationHasNoCppHeapActivity) {
  alignas(128) std::array<std::byte, 448> memory;
  std::array<SpscStatus, 10> statuses{};
  const auto activity = ObserveCppHeap([&]() noexcept {
    SpscQueue producer;
    SpscQueue consumer;
    statuses[0] = SpscQueue::RequiredBytes(64).status;
    statuses[1] = SpscQueue::Initialize(memory, 64);
    statuses[2] = producer.Attach(memory, Role::kProducer);
    statuses[3] = consumer.Attach(memory, Role::kConsumer);
    statuses[4] = producer.Detach();
    statuses[5] = consumer.Detach();
    statuses[6] = SpscQueue::RequiredBytes(63).status;
    statuses[7] = SpscQueue::Initialize(memory, 63);
    statuses[8] = producer.Attach({}, Role::kProducer);
    statuses[9] = producer.TryReserve(0).status;
  });
  for (std::size_t i = 0; i < 6; ++i) {
    EXPECT_EQ(statuses[i], SpscStatus::kSuccess) << "operation=" << i;
  }
  EXPECT_EQ(statuses[6], SpscStatus::kInvalidArgument);
  EXPECT_EQ(statuses[7], SpscStatus::kInvalidArgument);
  EXPECT_EQ(statuses[8], SpscStatus::kInvalidArgument);
  EXPECT_EQ(statuses[9], SpscStatus::kInvalidState);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

const char* ExerciseCopies(SpscQueue& producer, SpscQueue& consumer,
                           std::size_t& padding_only_returns) noexcept {
  std::array<std::byte, 60> input;
  std::array<std::byte, 60> output;
  for (std::size_t i = 0; i < input.size(); ++i) {
    input[i] = static_cast<std::byte>(i ^ 0x7a);
  }
  for (std::size_t iteration = 0; iteration < 1024; ++iteration) {
    const auto size = (iteration * 17) % 61;
    const auto payload = std::span(input).first(size);
    if (producer.CheckWrite(size) != SpscStatus::kSuccess) {
      return "copy coarse check";
    }
    auto status = producer.TryWrite(payload);
    if (status == SpscStatus::kFull) {
      ++padding_only_returns;
      if (consumer.TryAcquire().status != SpscStatus::kEmpty) {
        return "copy padding-only reclaim";
      }
      status = producer.TryWrite(payload);
    }
    if (status != SpscStatus::kSuccess) return "copy write";
    output.fill(std::byte{0x55});
    if (size != 0) {
      auto small = consumer.TryRead(std::span(output).first(size - 1));
      if (small.status != SpscStatus::kBufferTooSmall ||
          small.payload_size != size ||
          !std::all_of(output.begin(), output.end(), [](std::byte value) {
            return value == std::byte{0x55};
          })) {
        return "copy insufficient output";
      }
    }
    auto result = consumer.TryRead(output);
    if (result.status != SpscStatus::kSuccess || result.payload_size != size ||
        !std::equal(payload.begin(), payload.end(), output.begin())) {
      return "copy read";
    }
    if (consumer.TryRead(output).status != SpscStatus::kEmpty) {
      return "copy drained queue";
    }
  }
  return nullptr;
}

TEST_F(SpscQueueCppHeapTest, CopyAndWrapHaveNoCppHeapActivity) {
  const char* failure = nullptr;
  std::size_t padding_only_returns = 0;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure = ExerciseCopies(producer_, consumer_, padding_only_returns);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_GT(padding_only_returns, 0U);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

const char* ExerciseBorrows(SpscQueue& producer, SpscQueue& consumer,
                            SpscQueue& other_producer,
                            SpscQueue& other_consumer) noexcept {
  auto first = producer.TryReserve(32);
  auto second = other_producer.TryReserve(8);
  if (first.status != SpscStatus::kSuccess ||
      second.status != SpscStatus::kSuccess) {
    return "borrow reserve";
  }
  std::fill(first.reservation.payload().begin(),
            first.reservation.payload().end(), std::byte{0x2c});
  if (producer.TryReserve(0).status != SpscStatus::kBusy ||
      producer.CheckWrite(0) != SpscStatus::kBusy ||
      producer.TryWrite({}) != SpscStatus::kBusy ||
      producer.Detach() != SpscStatus::kBusy ||
      producer.TryAcquire().status != SpscStatus::kInvalidState ||
      first.reservation.Commit(33) != SpscStatus::kInvalidSize) {
    return "borrow write failures";
  }
  SpscQueue::WriteReservation moved(std::move(first.reservation));
  second.reservation = std::move(moved);
  if (first.reservation.active() || moved.active() ||
      other_producer.CheckWrite(0) != SpscStatus::kSuccess ||
      second.reservation.Commit(5) != SpscStatus::kSuccess ||
      second.reservation.Commit(0) != SpscStatus::kInvalidState) {
    return "write move and short commit";
  }
  const std::array<std::byte, 8> input{};
  if (other_producer.TryWrite(input) != SpscStatus::kSuccess) {
    return "second queue copy";
  }
  auto acquired = consumer.TryAcquire();
  auto other = other_consumer.TryAcquire();
  if (acquired.status != SpscStatus::kSuccess ||
      other.status != SpscStatus::kSuccess ||
      consumer.TryAcquire().status != SpscStatus::kBusy ||
      consumer.TryRead({}).status != SpscStatus::kBusy ||
      consumer.Detach() != SpscStatus::kBusy) {
    return "borrow read failures";
  }
  other.lease = std::move(acquired.lease);
  SpscQueue::ReadLease read(std::move(other.lease));
  if (acquired.lease.active() || other.lease.active() || read.size() != 5 ||
      read.Release() != SpscStatus::kSuccess ||
      read.Release() != SpscStatus::kInvalidState) {
    return "read move and release";
  }
  auto retained = other_consumer.TryAcquire();
  if (retained.status != SpscStatus::kSuccess) {
    return "move assignment retained original read frame";
  }
  retained.lease.Cancel();
  std::array<std::byte, 60> output{};
  if (other_consumer.TryRead(output).status != SpscStatus::kSuccess) {
    return "cancel retained frame";
  }
  {
    auto canceled = producer.TryReserve(16);
    if (canceled.status != SpscStatus::kSuccess) return "cancel reserve";
    canceled.reservation.payload()[0] = std::byte{0x64};
  }
  if (consumer.TryAcquire().status != SpscStatus::kEmpty ||
      producer.TryReserve(61).status != SpscStatus::kTooLarge) {
    return "canceled write and excessive size";
  }
  auto full = producer.TryReserve(60);
  if (full.status == SpscStatus::kFull) {
    if (consumer.TryAcquire().status != SpscStatus::kEmpty) {
      return "borrow padding-only reclaim";
    }
    full = producer.TryReserve(60);
  }
  if (full.status != SpscStatus::kSuccess) return "full-frame reserve";
  std::fill(full.reservation.payload().begin(),
            full.reservation.payload().end(), std::byte{0x35});
  if (full.reservation.Commit(60) != SpscStatus::kSuccess ||
      producer.TryWrite({}) != SpscStatus::kFull ||
      consumer.TryRead(std::span(output).first(8)).status !=
          SpscStatus::kBufferTooSmall ||
      consumer.TryRead(output).status != SpscStatus::kSuccess ||
      consumer.TryAcquire().status != SpscStatus::kEmpty) {
    return "full and insufficient-output failures";
  }
  return nullptr;
}

TEST_F(SpscQueueCppHeapTest, BorrowMoveCancelAndFailuresHaveNoCppHeapActivity) {
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure =
        ExerciseBorrows(producer_, consumer_, other_producer_, other_consumer_);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

}  // namespace
}  // namespace yquant::base
