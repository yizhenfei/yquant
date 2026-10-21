#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "yquant/base/logging.h"

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

TEST(LoggingCppHeapCounterTest, CountsReplacementOperatorCalls) {
  bool allocated = false;
  const auto activity = ObserveCppHeap([&]() noexcept {
    void* memory = ::operator new(16, std::nothrow);
    allocated = memory != nullptr;
    ::operator delete(memory, std::nothrow);
  });
  EXPECT_TRUE(allocated);
  EXPECT_EQ(activity.allocation_calls, 1U);
  EXPECT_EQ(activity.deallocation_calls, 1U);
}

int ReenterLogger(Logger& logger) noexcept {
  YQ_LOG_INFO(logger, "inner-reentry={}", 17);
  return 42;
}

void CheckLoggingHeap(bool shared) {
  char filename[] = "/tmp/yquant-log-allocation-XXXXXX";
  const int fd = mkstemp(filename);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(close(fd), 0);
  LogManager manager;
  LogConfig manager_config;
  if (shared) manager_config.backend = LogBackend::kLogd;
  ASSERT_TRUE(manager.Initialize(manager_config).ok());
  LoggerConfig config;
  config.name = "allocation";
  config.file_path = filename;
  config.queue_capacity = 1U << 20;
  LogManager sink;
  char queue_path[128];
  if (shared) {
    ASSERT_TRUE(sink.Initialize().ok());
    std::snprintf(queue_path, sizeof(queue_path), "%s.queue", filename);
    config.queue_path = queue_path;
    ASSERT_TRUE(sink.CreateLogSink(config).ok());
    config.file_path = {};
  }
  const auto created = manager.CreateLogger(config);
  ASSERT_TRUE(created.status.ok());
  ASSERT_NE(created.logger, nullptr);
  Logger& logger = *created.logger;
  std::array<char, kMaxLogInputBytes + 1> input{};
  input.fill('x');
  const auto normal = std::string_view(input.data(), 32);
  const auto truncated = std::string_view(input.data(), 2 * kMaxLogBytes);
  const auto rejected = std::string_view(input.data(), input.size());
  const auto activity = ObserveCppHeap([&]() noexcept {
    YQ_LOG_INFO(logger, "{} {} {} {} {} {} {} {} {}", true, 'x', -1, 2U, 1.25F,
                2.5, std::string_view("view"), "literal", normal);
    YQ_LOG_INFO(logger, "{}", truncated);
    YQ_LOG_INFO(logger, "{}", rejected);
    YQ_LOG_INFO(logger, "outer={}", ReenterLogger(logger));
    for (int index = 0; index < 10000; ++index) {
      YQ_LOG_INFO(logger, "steady={} {} {}", index, 3.25, normal);
    }
    logger.SetMinimumLevel(LogLevel::kOff);
    YQ_LOG_CRITICAL(logger, "runtime-filtered={}", 17);
    logger.SetMinimumLevel(LogLevel::kInfo);
  });
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  const auto counts = logger.counters();
  EXPECT_GT(counts.enqueued_records_total, 0U);
  EXPECT_EQ(counts.input_rejected_records_total, 1U);
  EXPECT_EQ(counts.reentrant_records_total, 1U);
  EXPECT_EQ(counts.truncated_records_total, 1U);
  EXPECT_TRUE(manager.Shutdown().status.ok());
  EXPECT_TRUE(sink.Shutdown().status.ok());
  EXPECT_EQ(unlink(filename), 0);
}

TEST(LoggingCppHeapTest, FirstCallAndSteadyStateHaveNoCppHeapActivity) {
  CheckLoggingHeap(false);
}

TEST(LoggingCppHeapTest, LogdFirstCallAndSteadyStateHaveNoCppHeapActivity) {
  CheckLoggingHeap(true);
}

}  // namespace
}  // namespace yquant::base
