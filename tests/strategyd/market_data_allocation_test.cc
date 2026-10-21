#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <utility>

#include "gtest/gtest.h"
#include "strategyd/market_data.h"
#include "strategyd/strategy_context.h"
#include "yquant/md/mock_quote_source.h"
#include "yquant/model/identifiers.h"
#include "yquant/trading/trading.h"

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
class ManualClock final : public base::MonotonicClock {
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
const char* ExerciseMarketData(md::MockQuoteSource& source,
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
    if (source.Poll(clock.Now(), tick).state != md::MockQuotePollState::kIdle ||
        tick != idle_output) {
      return "idle polling changed the output or produced early data";
    }
    ++counts.idle;
    ++clock.now;
    for (std::size_t instrument = 0; instrument < instrument_count;
         ++instrument) {
      const auto result = source.Poll(clock.Now(), tick);
      if (result.state != md::MockQuotePollState::kEvent ||
          !result.status.ok()) {
        return "source failed to produce the next scheduled quote";
      }
      bool accepted = false;
      if (market_data.ApplyQuote(tick, accepted) != base::ErrorCode::kOk ||
          !accepted) {
        return "context did not accept a fresh quote";
      }
      ++counts.quotes;
      market_data.BeginCallback(strategy::CallbackPhase::kTick);
      model::QuoteTick snapshot;
      if (!context.market_data().TryGetLatestQuote(tick.instrument_id,
                                                   snapshot) ||
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
      if (!context.market_data().TryGetLatestQuote(tick.instrument_id,
                                                   snapshot) ||
          snapshot != tick) {
        return "ignored or invalid records changed the latest snapshot";
      }
      const auto unchanged = snapshot;
      if (context.market_data().TryGetLatestQuote({}, snapshot) ||
          snapshot != unchanged ||
          context.market_data().TryGetLatestQuote(model::InstrumentId{1},
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
  md::MockQuoteConfig config;
  config.source_id = {17};
  config.interval_ns = 1000000;
  config.instrument_count = md::kMaxMarketDataInstruments;
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    auto& item = config.instruments[index];
    item.instrument.venue = model::Venue::kSse;
    std::size_t numeric_symbol = 600000 + index;
    for (std::size_t digit = model::kSymbolLength; digit != 0; --digit) {
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
  trading::TradingImpl trading(clock, market_data.instruments());
  StrategyContextImpl context(clock, market_data, trading);
  md::MarketDataConfig host;
  host.source_id = config.source_id;
  host.instrument_count = config.instrument_count;
  for (std::size_t i = 0; i < host.instrument_count; ++i)
    host.instruments[i].instrument = config.instruments[i].instrument;
  ASSERT_TRUE(market_data.Configure(host).ok());
  market_data.BeginCallback(strategy::CallbackPhase::kStart);
  for (std::size_t index = 0; index < config.instrument_count; ++index) {
    ASSERT_EQ(context.market_data().SubscribeQuotes(
                  config.instruments[index].instrument.id),
              base::ErrorCode::kOk);
  }
  market_data.EndCallback();
  md::MockQuoteSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start(clock.Now()).ok());

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
  EXPECT_EQ(counts.quotes, 128U * md::kMaxMarketDataInstruments);
  EXPECT_EQ(counts.idle, 128U);
  EXPECT_EQ(counts.duplicates, counts.quotes);
  EXPECT_EQ(counts.old_quotes, 127U * md::kMaxMarketDataInstruments);
  EXPECT_EQ(counts.rejected, counts.quotes);
  EXPECT_EQ(counts.query_misses, 2 * counts.quotes);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  EXPECT_TRUE(source.Stop().ok());
}

}  // namespace
}  // namespace yquant::strategyd
