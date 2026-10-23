#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <system_error>
#include <utility>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "yquant/base/broadcast_queue.h"
#include "yquant/base/shm_broadcast.h"

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

namespace yquant::base {
namespace {
TEST(BroadcastQueueCppHeapTest, InitializationAttachAndHotPathsDoNotAllocate) {
  alignas(128) std::array<std::byte, 4096> memory;
  std::array<std::byte, 32> scratch{};
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    BroadcastWriter writer;
    BroadcastReader reader;
    if (!BroadcastQueue::Initialize(memory, {4, 32}, {1, 2}).ok() ||
        !writer.Attach(memory).ok() || !reader.Attach(memory, scratch).ok()) {
      failure = "initialization/attach";
      return;
    }
    for (int i = 0; i < 100; ++i) {
      for (int j = 0; j < 8; ++j) {
        if (!writer.TryWrite(std::span<const std::byte>(scratch)).ok()) {
          failure = "write";
          return;
        }
      }
      auto read = reader.TryRead();
      if (read.status != BroadcastReadStatus::kGap || !read.gap) {
        failure = "gap";
        return;
      }
      uint64_t sequence;
      if (!reader.Resume(*read.gap, sequence, BroadcastResumePosition::Live())
               .ok()) {
        failure = "resume";
        return;
      }
      if (!writer.TryWrite({}).ok() ||
          reader.TryRead().status != BroadcastReadStatus::kRecord) {
        failure = "read";
        return;
      }
    }
    if (!writer.Finish().ok() ||
        reader.TryRead().status != BroadcastReadStatus::kEnd) {
      failure = "finish";
    }
    reader.Detach();
    writer.Detach();
  });
  EXPECT_EQ(failure, nullptr) << (failure ? failure : "");
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

TEST(BroadcastQueueCppHeapTest, SharedMemoryByteHotPathsDoNotAllocate) {
  char directory[] = "/tmp/yquant-broadcast-heap-XXXXXX";
  ASSERT_NE(::mkdtemp(directory), nullptr);
  ShmBroadcastWriter writer;
  ShmBroadcastReader reader;
  std::array<std::byte, 32> scratch{};
  ASSERT_TRUE(writer.Create({{directory, "bytes"}, {4, 32}}).ok());
  ASSERT_TRUE(reader.Open({directory, "bytes"}, scratch).ok());
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    for (int i = 0; i < 100; ++i) {
      if (!writer.TryWrite({}).ok() ||
          reader.TryRead().read.status != BroadcastReadStatus::kRecord) {
        failure = "shared-memory read/write";
        return;
      }
    }
  });
  EXPECT_EQ(failure, nullptr);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  reader.Close();
  writer.Close();
  std::error_code error;
  ::yquant::testing::files::RemoveTree(
      ::yquant::testing::files::FilePath(directory), error);
}
}  // namespace
}  // namespace yquant::base
