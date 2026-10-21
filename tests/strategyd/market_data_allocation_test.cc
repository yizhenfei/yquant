#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <utility>

#include "gtest/gtest.h"
#include "strategyd/market_data.h"
#include "strategyd/mock_market_data_source.h"
#include "strategyd/replay_market_data_source.h"
#include "strategyd/strategy_context.h"
#include "strategyd/timer.h"
#include "strategyd/trading.h"
#include "test_config_file.h"

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

namespace yquant::strategyd {
namespace {

// Test clock shares the source/context time domain without wall-clock sleeps.
class ManualClock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 0;
};

struct ExerciseCounts {
  std::size_t quotes = 0;
  std::size_t idle = 0;
  std::size_t duplicates = 0;
  std::size_t old_quotes = 0;
  std::size_t rejected = 0;
  std::size_t query_misses = 0;
};

// Performs only allocation-counted work. A literal failure reason and counters
// defer all GoogleTest reporting until after the observation window closes.
const char* ExerciseMarketData(MockMarketDataSource& source,
                               MarketDataImpl& market_data,
                               const strategy::StrategyContext& context,
                               ManualClock& clock, std::uint64_t interval,
                               std::size_t instrument_count,
                               ExerciseCounts& counts) noexcept {
  constexpr std::size_t kBatches = 128;
  model::QuoteTick tick;
  tick.sequence = 12345;
  for (std::size_t batch = 0; batch < kBatches; ++batch) {
    const auto idle_output = tick;
    clock.now += interval - 1;
    if (source.Poll(clock.Now(), tick).state != MarketDataPollState::kIdle ||
        tick != idle_output) {
      return "idle polling changed the output or produced early data";
    }
    ++counts.idle;
    ++clock.now;
    for (std::size_t instrument = 0; instrument < instrument_count;
         ++instrument) {
      const auto result = source.Poll(clock.Now(), tick);
      if (result.state != MarketDataPollState::kEvent || !result.status.ok()) {
        return "source failed to produce the next scheduled quote";
      }
      bool accepted = false;
      if (market_data.ApplyQuote(tick, accepted) != base::ErrorCode::kOk ||
          !accepted) {
        return "context did not accept a fresh quote";
      }
      ++counts.quotes;
      market_data.BeginCallback(CallbackPhase::kTick);
      model::QuoteTick snapshot;
      if (!context.market_data().TryGetLatestQuote(
              tick.source_id, tick.instrument_id, snapshot) ||
          snapshot != tick || context.Now() != tick.receive_time_ns) {
        return "callback query differs from the quote just published";
      }
      market_data.EndCallback();

      auto replay = tick;
      // Valid but different prices ensure replay handling preserves the cache.
      replay.bid_price = 1;
      replay.ask_price = 2;
      accepted = true;
      if (market_data.ApplyQuote(replay, accepted) != base::ErrorCode::kOk ||
          accepted) {
        return "duplicate quote was not ignored";
      }
      ++counts.duplicates;
      if (replay.sequence > 1) {
        --replay.sequence;
        accepted = true;
        if (market_data.ApplyQuote(replay, accepted) != base::ErrorCode::kOk ||
            accepted) {
          return "old quote was not ignored";
        }
        ++counts.old_quotes;
      }

      auto invalid = tick;
      invalid.source_id = {0};
      accepted = true;
      if (market_data.ApplyQuote(invalid, accepted) !=
              base::ErrorCode::kCorrupt ||
          !accepted) {
        return "invalid source changed acceptance or was not rejected";
      }
      ++counts.rejected;
      if (!context.market_data().TryGetLatestQuote(
              tick.source_id, tick.instrument_id, snapshot) ||
          snapshot != tick) {
        return "ignored or invalid records changed the latest snapshot";
      }
      const auto unchanged = snapshot;
      if (context.market_data().TryGetLatestQuote({}, tick.instrument_id,
                                                  snapshot) ||
          snapshot != unchanged ||
          context.market_data().TryGetLatestQuote(tick.source_id, {},
                                                  snapshot) ||
          snapshot != unchanged) {
        return "failed lookup changed its output";
      }
      counts.query_misses += 2;
    }
    if (source.next_deadline() != clock.Now() + interval) {
      return "batch completion did not schedule the next interval";
    }
  }
  return nullptr;
}

TEST(MarketDataCppHeapCounterTest, CountsReplacementOperatorCalls) {
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
    for (const auto pointer : pointers) succeeded &= pointer != nullptr;
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

TEST(MarketDataCppHeapTest, FirstPollAndSteadyDispatchHaveNoCppHeapActivity) {
  MarketDataConfig config;
  config.source_id = {17};
  config.interval_ns = 1000000;
  config.instrument_count = kMaxMarketDataInstruments;
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    auto& item = config.instruments[index];
    item.instrument.venue = model::Venue::kSse;
    std::size_t numeric_symbol = 600000 + index;
    for (std::size_t digit = 6; digit != 0; --digit) {
      item.instrument.symbol[digit - 1] = '0' + numeric_symbol % 10;
      numeric_symbol /= 10;
    }
    ASSERT_EQ(model::MakeInstrumentId("SSE", item.instrument.symbol_view(),
                                      item.instrument.id),
              base::ErrorCode::kOk);
    item.instrument.price_decimals = 2;
    item.bid_price = 1000 + static_cast<std::int64_t>(index);
    item.ask_price = item.bid_price + 1;
    item.bid_qty = 100;
    item.ask_qty = 200;
    item.price_step = 3;
  }
  ManualClock clock;
  MarketDataImpl market_data(clock);
  TradingImpl trading(clock, market_data);
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  ASSERT_TRUE(market_data.Configure(config).ok());
  market_data.BeginCallback(CallbackPhase::kStart);
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    ASSERT_EQ(context.market_data().SubscribeQuotes(
                  config.source_id, config.instruments[index].instrument.id),
              base::ErrorCode::kOk);
  }
  market_data.EndCallback();
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start(market_data.subscriptions(), clock.Now()).ok());

  ExerciseCounts counts;
  const char* failure = nullptr;
  // Includes the first call to Poll/system_clock::now; no hot-path warmup hides
  // one-time C++ allocations. Initialization and assertion output stay outside.
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure =
        ExerciseMarketData(source, market_data, context, clock,
                           config.interval_ns, config.instrument_count, counts);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(counts.quotes, 128U * kMaxMarketDataInstruments);
  EXPECT_EQ(counts.idle, 128U);
  EXPECT_EQ(counts.duplicates, counts.quotes);
  EXPECT_EQ(counts.old_quotes, 127U * kMaxMarketDataInstruments);
  EXPECT_EQ(counts.rejected, counts.quotes);
  EXPECT_EQ(counts.query_misses, 2 * counts.quotes);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  EXPECT_TRUE(source.Stop().ok());
}

TEST(MarketDataCppHeapTest,
     ReplayStartPollingAndCompletionHaveNoCppHeapActivity) {
  testing::ConfigFile csv(
      "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n"
      "0,0,1000,100,1001,200,7\n4,0,1000,100,1001,200,7\n"
      "4,0,1000,100,1001,200,7\n10,0,1000,100,1001,200,7\n");
  ASSERT_TRUE(csv.valid());
  MarketDataConfig config;
  config.adapter = MarketDataAdapter::kReplay;
  config.source_id = {1};
  config.instrument_count = 1;
  config.replay_speed = 2;
  config.replay_tail_ns = 5;
  std::strcpy(config.replay_path, csv.path());
  auto& definition = config.instruments[0].instrument;
  definition.venue = model::Venue::kSse;
  std::memcpy(definition.symbol, "600000", 6);
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", definition.id),
            base::ErrorCode::kOk);
  const auto id = definition.id;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok()) << source.error_detail();
  const char* failure = nullptr;
  std::size_t quotes = 0;
  const auto activity = ObserveCppHeap([&]() noexcept {
    if (!source.Start(std::span(&id, 1), 100).ok() ||
        source.next_deadline() != 100 || source.finished()) {
      failure = "replay did not start with its first due frame";
      return;
    }
    model::QuoteTick tick;
    for (std::uint64_t now = 100; now <= 110; ++now) {
      for (;;) {
        const auto previous = tick;
        const auto result = source.Poll(now, tick);
        if (!result.status.ok()) {
          failure = "replay polling failed";
          return;
        }
        if (result.state == MarketDataPollState::kIdle) {
          if (tick != previous) failure = "idle replay changed output";
          break;
        }
        if (result.state != MarketDataPollState::kEvent ||
            tick.sequence != ++quotes || tick.receive_time_ns != now) {
          failure = "replay frame ordering or receipt timestamp changed";
          return;
        }
      }
      if (source.finished() != (now == 110)) {
        failure = "replay completed before or after its tail deadline";
        return;
      }
    }
    if (source.next_deadline()) failure = "finished replay retained a deadline";
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(quotes, 4U);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  EXPECT_TRUE(source.Stop().ok());
}

}  // namespace
}  // namespace yquant::strategyd
