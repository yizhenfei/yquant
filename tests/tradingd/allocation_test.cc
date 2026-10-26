#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <thread>
#include <utility>

#include "gtest/gtest.h"
#include "tradingd/server.h"
#include "yquant/trading/connection.h"

namespace {

struct CppHeapActivity {
  size_t allocation_calls = 0;
  size_t deallocation_calls = 0;
};

constinit thread_local bool g_track_cpp_heap = false;
constinit thread_local CppHeapActivity g_cpp_heap_activity;

// Counts replacement C++ allocation functions on the observed owning thread.
// Direct malloc/free and OS mappings are outside this counter's scope.
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

namespace yquant::tradingd {
namespace {

class TestClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000000;
};

struct Observation {
  uint64_t generation = 0;
  uint64_t previous_revision = 0;
  bool saw_account = false;
  bool saw_accepted = false;
  bool saw_fill = false;
  bool saw_canceled = false;
  bool saw_fill_during_cancel = false;
  bool revision_regressed = false;
  model::Fill fill;
  model::OrderSnapshot canceled;
  trading::AccountView account;
};

// Owns all values needed for assertions; the observation path never calls
// GoogleTest or grows a container while allocation tracking is enabled.
void Capture(const trading::Message& message,
             Observation& observation) noexcept {
  if (message.type == trading::MessageType::kWelcome) {
    observation.generation = message.generation;
  }
  if (message.type == trading::MessageType::kWelcome ||
      message.type == trading::MessageType::kAccount ||
      message.type == trading::MessageType::kOrderUpdate) {
    observation.revision_regressed |=
        message.account.revision < observation.previous_revision;
    observation.previous_revision = message.account.revision;
    observation.account = message.account;
  }
  if (message.type == trading::MessageType::kAccount) {
    observation.saw_account = true;
  }
  if (message.type != trading::MessageType::kOrderUpdate) {
    return;
  }
  if (message.update.kind == model::OrderUpdateKind::kAccepted) {
    observation.saw_accepted = true;
  }
  if (message.has_fill) {
    observation.saw_fill = true;
    observation.fill = message.fill;
    observation.saw_fill_during_cancel |= message.update.order.cancel_pending;
  }
  if (message.update.kind == model::OrderUpdateKind::kCanceled) {
    observation.saw_canceled = true;
    observation.canceled = message.update.order;
  }
}

// Drives one established connection on the same thread as its server, so the
// counter sees encoding, kernel I/O, account changes and complete decoding.
const char* Cycle(Server& server, trading::Connection& client,
                  const TestClock& clock, Observation& observation) noexcept {
  if (!client.Service().ok()) {
    return "client service failed";
  }
  if (!server.Service(clock.now).ok()) {
    return "server service failed";
  }
  for (size_t count = 0; count < trading::kConnectionQueueFrames; ++count) {
    trading::Message message;
    const auto result = client.Poll(message);
    if (result.state == trading::ReceiveState::kIdle) {
      break;
    }
    if (result.state != trading::ReceiveState::kMessage) {
      return "client receive failed";
    }
    Capture(message, observation);
  }
  return nullptr;
}

template <typename Done>
const char* PumpUntil(Server& server, trading::Connection& client,
                      const TestClock& clock, Observation& observation,
                      Done done) noexcept {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!done()) {
    if (const auto* failure = Cycle(server, client, clock, observation)) {
      return failure;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return "message deadline expired";
    }
    std::this_thread::yield();
  }
  return nullptr;
}

// Sending the cancel between acceptance and the first fill deliberately lets
// a partial fill arrive with cancel_pending set. Its earlier cancel deadline
// then removes the final-fill plan and releases the unfilled reservation.
const char* Exercise(Server& server, trading::Connection& client,
                     TestClock& clock, model::InstrumentId instrument,
                     Observation& observation) noexcept {
  trading::Message order;
  order.type = trading::MessageType::kNewOrder;
  order.generation = observation.generation;
  order.strategy_order_id = {1};
  order.request = {instrument, model::OrderSide::kBuy, 1000, 10};
  if (!client.Queue(order).ok()) {
    return "order queue failed";
  }
  if (const auto* failure = PumpUntil(server, client, clock, observation, [&] {
        return observation.account.revision >= 2;
      })) {
    return failure;
  }
  clock.now += 1000000;
  if (const auto* failure = PumpUntil(server, client, clock, observation, [&] {
        return observation.saw_accepted;
      })) {
    return failure;
  }
  clock.now += 500000;
  trading::Message cancel;
  cancel.type = trading::MessageType::kCancelOrder;
  cancel.generation = observation.generation;
  cancel.strategy_order_id = {1};
  cancel.cancel_id = {1};
  if (!client.Queue(cancel).ok()) {
    return "cancel queue failed";
  }
  for (size_t count = 0; count < trading::kConnectionQueueFrames; ++count) {
    if (const auto* failure = Cycle(server, client, clock, observation)) {
      return failure;
    }
  }
  clock.now += 500000;
  if (const auto* failure = PumpUntil(server, client, clock, observation,
                                      [&] { return observation.saw_fill; })) {
    return failure;
  }
  clock.now += 500000;
  return PumpUntil(server, client, clock, observation,
                   [&] { return observation.saw_canceled; });
}

TEST(TradingdCppHeapCounter, CountsAllReplacementAllocationForms) {
  std::array<void*, 8> pointers{};
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
  EXPECT_EQ(activity.allocation_calls, 8U);
  EXPECT_EQ(activity.deallocation_calls, 8U);
}

// Runs the full gateway path after authentication.
void CheckEstablishedTradingHeap() {
  TestClock clock;
  ServerConfig config;
  config.listen.port = 0;
  std::memcpy(config.token, "allocation-test", 16);
  config.market_data.source_id = {1};

  config.market_data.instrument_count = 1;
  auto& item = config.market_data.instruments[0];
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
            base::ErrorCode::kOk);
  item.instrument.venue = model::Venue::kSse;
  std::memcpy(item.instrument.symbol, "600000", model::kSymbolLength);
  item.instrument.price_decimals = 2;

  config.trading.mode = trading::MockTradingMode::kFill;
  config.trading.report_interval_ns = 1000000;
  config.trading.initial_account.cash_balance = 1000000000;
  auto server = std::unique_ptr<Server>(new (std::nothrow) Server(&clock));
  ASSERT_NE(server, nullptr);
  ASSERT_TRUE(server->Init(config).ok()) << server->error_detail();
  trading::Endpoint endpoint;
  endpoint.port = server->port();
  trading::Connection client;
  ASSERT_TRUE(client.Connect(endpoint, 1000000000).ok());
  trading::Message hello;
  hello.type = trading::MessageType::kHello;
  hello.client_id = 41;
  std::memcpy(hello.token, config.token, sizeof(hello.token));
  hello.instrument_count = 1;
  hello.instruments[0] = {item.instrument.id, 2};
  ASSERT_TRUE(client.Queue(hello).ok());
  Observation observation;
  ASSERT_EQ(PumpUntil(*server, client, clock, observation,
                      [&] { return observation.generation != 0; }),
            nullptr);
  ASSERT_EQ(observation.account.cash.balance, 1000000000);
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure = Exercise(*server, client, clock, item.instrument.id, observation);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_TRUE(observation.saw_account);
  EXPECT_TRUE(observation.saw_accepted);
  EXPECT_TRUE(observation.saw_fill);
  EXPECT_TRUE(observation.saw_fill_during_cancel);
  EXPECT_TRUE(observation.saw_canceled);
  EXPECT_FALSE(observation.revision_regressed);
  EXPECT_EQ(observation.fill.quantity, 5);
  EXPECT_EQ(observation.fill.price, 1000);
  EXPECT_EQ(observation.canceled.cumulative_filled_qty, 5);
  EXPECT_EQ(observation.canceled.leaves_qty, 0);
  EXPECT_FALSE(observation.canceled.cancel_pending);
  EXPECT_EQ(observation.account.cash.balance, 950000000);
  EXPECT_EQ(observation.account.cash.reserved, 0);
  EXPECT_EQ(observation.account.cash.available, 950000000);
  ASSERT_EQ(observation.account.position_count, 1U);
  EXPECT_EQ(observation.account.positions[0].total_qty, 5);
  EXPECT_EQ(observation.account.positions[0].available_qty, 0);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

TEST(TradingdCppHeap, EstablishedOrderFillAndCancelHaveNoCppHeapActivity) {
  CheckEstablishedTradingHeap();
}

}  // namespace
}  // namespace yquant::tradingd
