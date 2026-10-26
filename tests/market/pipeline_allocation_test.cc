#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "gtest/gtest.h"
#include "market/pipeline_test_support.h"
#include "yquant/base/format.h"
#include "yquant/market/recording.h"
#include "yquant/market/replay_source.h"
#include "yquant/market/sources.h"
#include "yquant/market/tcp_transport.h"

namespace {

struct CppHeapActivity {
  size_t allocation_calls = 0;
  size_t deallocation_calls = 0;
};

constinit thread_local bool g_track_cpp_heap = false;
constinit thread_local CppHeapActivity g_cpp_heap_activity;

// This executable counts replacement C++ allocation/deallocation functions.
// Direct calls to malloc, realloc, free, or OS mapping APIs are not counted.
void* AllocateCppHeap(size_t size, size_t alignment,
                      bool abort_on_failure) noexcept {
  if (g_track_cpp_heap) {
    ++g_cpp_heap_activity.allocation_calls;
  }
  size = std::max<size_t>(size, 1);
  void* result = nullptr;
  if (alignment == 0) {
    result = std::malloc(size);
  } else if (posix_memalign(&result, std::max(alignment, alignof(void*)),
                            size) != 0) {
    result = nullptr;
  }
  if (result == nullptr && abort_on_failure) {
    std::abort();
  }
  return result;
}

void DeallocateCppHeap(void* memory) noexcept {
  if (g_track_cpp_heap) {
    ++g_cpp_heap_activity.deallocation_calls;
  }
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

void* operator new(size_t size) { return AllocateCppHeap(size, 0, true); }

void* operator new[](size_t size) { return AllocateCppHeap(size, 0, true); }

void* operator new(size_t size, const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, 0, false);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, 0, false);
}

void* operator new(size_t size, std::align_val_t alignment) {
  return AllocateCppHeap(size, static_cast<size_t>(alignment), true);
}

void* operator new[](size_t size, std::align_val_t alignment) {
  return AllocateCppHeap(size, static_cast<size_t>(alignment), true);
}

void* operator new(size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, static_cast<size_t>(alignment), false);
}

void* operator new[](size_t size, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  return AllocateCppHeap(size, static_cast<size_t>(alignment), false);
}

void operator delete(void* memory) noexcept { DeallocateCppHeap(memory); }

void operator delete[](void* memory) noexcept { DeallocateCppHeap(memory); }

void operator delete(void* memory, size_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, size_t) noexcept {
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

void operator delete(void* memory, size_t, std::align_val_t) noexcept {
  DeallocateCppHeap(memory);
}

void operator delete[](void* memory, size_t, std::align_val_t) noexcept {
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

namespace yquant::market {
namespace {

constexpr size_t kQuoteCount = 128;

// Caller completes initialization before observation. Every event includes
// first-use quote encoding/decoding; the prior idle Poll only refreshes
// discovery.
const char* ExerciseShm(ShmPublisher& publisher, ShmSource& source,
                        model::QuoteTick quote, size_t& received) noexcept {
  model::QuoteTick output;
  for (uint64_t sequence = 1; sequence <= kQuoteCount; ++sequence) {
    quote.sequence = sequence;
    if (!publisher.Publish(quote).ok()) {
      return "shared-memory publish failed";
    }
    const auto result = source.Poll(sequence, output);
    auto expected = quote;
    expected.receive_time_ns = sequence;
    if (result.state != PollState::kEvent || !result.status.ok() ||
        output != expected) {
      return "shared-memory quote did not match the published event";
    }
    ++received;
    const auto before = output;
    const auto idle = source.Poll(sequence, output);
    if (idle.state != PollState::kIdle || !idle.status.ok() ||
        output != before) {
      return "shared-memory idle poll changed its output";
    }
  }
  return nullptr;
}

// Accept/connect setup occurs before observation. The exercise includes first
// quote delivery, idle services, repeated publications and nonblocking polls.
const char* ExerciseTcp(TcpPublisher& publisher, TcpSource& source,
                        model::QuoteTick quote, size_t& received) noexcept {
  model::QuoteTick output;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  for (uint64_t sequence = 1; sequence <= kQuoteCount; ++sequence) {
    quote.sequence = sequence;
    if (!publisher.Publish(quote).ok()) {
      return "TCP publish failed";
    }
    PollResult result;
    while (std::chrono::steady_clock::now() < deadline) {
      if (!publisher.Service(sequence).ok()) {
        return "TCP service failed";
      }
      result = source.Poll(sequence, output);
      if (result.state != PollState::kIdle) {
        break;
      }
      std::this_thread::yield();
    }
    auto expected = quote;
    expected.receive_time_ns = sequence;
    if (result.state != PollState::kEvent || !result.status.ok() ||
        output != expected) {
      return "TCP quote did not arrive intact before the deadline";
    }
    ++received;
    const auto before = output;
    const auto idle = source.Poll(sequence, output);
    if (idle.state != PollState::kIdle || !idle.status.ok() ||
        output != before) {
      return "TCP idle poll changed its output";
    }
  }
  return nullptr;
}

TEST(MarketdCppHeapCounterTest, CountsReplacementOperatorCalls) {
  std::array<void*, 8> pointers = {};
  bool succeeded = true;
  const auto activity = ObserveCppHeap([&]() noexcept {
    pointers[0] = ::operator new(16);
    pointers[1] = ::operator new[](16);
    pointers[2] = ::operator new(16, std::nothrow);
    pointers[3] = ::operator new[](16, std::nothrow);
    pointers[4] = ::operator new(16, std::align_val_t{128});
    pointers[5] = ::operator new[](16, std::align_val_t{128});
    pointers[6] = ::operator new(16, std::align_val_t{128}, std::nothrow);
    pointers[7] = ::operator new[](16, std::align_val_t{128}, std::nothrow);
    for (const auto pointer : pointers) {
      succeeded &= pointer != nullptr;
    }
    ::operator delete(pointers[0]);
    ::operator delete[](pointers[1]);
    ::operator delete(pointers[2], std::nothrow);
    ::operator delete[](pointers[3], std::nothrow);
    ::operator delete(pointers[4], std::align_val_t{128});
    ::operator delete[](pointers[5], std::align_val_t{128});
    ::operator delete(pointers[6], std::align_val_t{128}, std::nothrow);
    ::operator delete[](pointers[7], std::align_val_t{128}, std::nothrow);
  });
  EXPECT_TRUE(succeeded);
  EXPECT_EQ(activity.allocation_calls, pointers.size());
  EXPECT_EQ(activity.deallocation_calls, pointers.size());
}

TEST(MarketdCppHeapTest, ShmFirstQuoteAndSteadyDeliveryHaveNoCppHeapActivity) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const market::Endpoint endpoint{directory.path(), "allocation-quotes"};
  ShmPublisher publisher;
  ASSERT_TRUE(publisher.Init({endpoint, 256, 256}).ok());
  ShmSource source;
  ASSERT_TRUE(source.Init(endpoint).ok());
  model::QuoteTick idle_output;
  // Discovery refresh and heartbeat service are control-path work. No quote is
  // published or decoded before the allocation observation begins.
  ASSERT_TRUE(publisher.Service(0).ok());
  ASSERT_EQ(source.Poll(0, idle_output).state, PollState::kIdle);
  const auto quote = testing::Quote();
  size_t received = 0;
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure = ExerciseShm(publisher, source, quote, received);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(received, kQuoteCount);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

TEST(MarketdCppHeapTest, TcpFirstQuoteAndSteadyDeliveryHaveNoCppHeapActivity) {
  TcpPublisher publisher;
  ASSERT_TRUE(publisher.Init({}).ok()) << publisher.error_detail();
  TcpSource source;
  ASSERT_TRUE(source.Init({"127.0.0.1", publisher.port()}).ok());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (publisher.connected_clients() != 1 &&
         std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(publisher.Service(0).ok());
    std::this_thread::yield();
  }
  ASSERT_EQ(publisher.connected_clients(), 1);
  model::QuoteTick idle_output;
  ASSERT_EQ(source.Poll(0, idle_output).state, PollState::kIdle);
  const auto quote = testing::Quote();
  size_t received = 0;
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure = ExerciseTcp(publisher, source, quote, received);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(received, kQuoteCount);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

TEST(MarketdCppHeapTest, ReplayAndRecordingHaveNoCppHeapActivity) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  std::string csv =
      "Date,Time,SecurityID,SecurityIDSource,BidPX1,BidSize1,OfferPX1,"
      "OfferSize1\n";
  for (size_t i = 0; i < kQuoteCount; ++i) {
    csv += "2020.12.31,09:15:00.000,600000,101,10,100,10.01,200\n";
  }
  ASSERT_TRUE(directory.Write("quotes.csv", csv));
  ReplayConfig input;
  ::yquant::base::FormatTo(input.path, sizeof(input.path), "{}",
                           directory.File("quotes.csv").c_str());
  input.format = ReplayFormat::kDolphinDbCsv;
  ::yquant::base::FormatTo(input.instrument_catalog,
                           sizeof(input.instrument_catalog), "{}",
                           directory.File("instruments.json").c_str());
  input.speed = 0;
  input.max_records = kQuoteCount;
  ReplaySource source;
  ASSERT_TRUE(source.Init(input).ok()) << source.error_detail();
  RecordConfig output;
  ::yquant::base::FormatTo(output.path, sizeof(output.path), "{}",
                           directory.File("quotes.yqr").c_str());
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(output).ok()) << publisher.error_detail();
  bool succeeded = true;
  size_t count = 0;
  const auto activity = ObserveCppHeap([&]() noexcept {
    model::QuoteTick quote;
    for (; count < kQuoteCount; ++count) {
      if (source.Poll(count, quote).state != PollState::kEvent ||
          !publisher.Publish(quote).ok()) {
        succeeded = false;
        break;
      }
    }
    succeeded &= source.Poll(count, quote).state == PollState::kEnd;
    succeeded &= publisher.Service(count).ok();
  });
  EXPECT_TRUE(succeeded);
  EXPECT_EQ(count, kQuoteCount);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  EXPECT_TRUE(publisher.Finish().ok()) << publisher.error_detail();
}

}  // namespace
}  // namespace yquant::market
