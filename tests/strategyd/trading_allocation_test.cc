#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <span>
#include <utility>

#include "gtest/gtest.h"
#include "strategyd/market_data.h"
#include "strategyd/mock_trading_adapter.h"
#include "strategyd/simulated_futures_exchange.h"
#include "strategyd/strategy_context.h"
#include "strategyd/timer.h"
#include "strategyd/trading.h"
#include "strategyd/trading_adapter.h"

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

class TradingHeapClock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 0;
};

// Runs accepted requests, idle checks, normalized reports and callback-side
// queries without test reporting inside the allocation observation window.
const char* ExerciseOrders(TradingImpl& trading, MockTradingAdapter& adapter,
                           StrategyContextImpl& context,
                           TradingHeapClock& clock,
                           model::InstrumentId instrument, bool cancel,
                           bool sell, std::size_t& fill_count) noexcept {
  constexpr std::size_t kOrders = 128;
  constexpr std::int64_t kUnitCash = 10010000;
  constexpr std::int64_t kReservation = kUnitCash * 201;
  const strategy::StrategyContext& read_only = context;
  model::CashSnapshot initial_cash;
  model::PositionSnapshot initial_position;
  if (!read_only.trading().TryGetCash(initial_cash) ||
      !read_only.trading().TryGetPosition(instrument, initial_position)) {
    return "configured account was not queryable";
  }
  std::int64_t filled_qty = 0;
  for (std::size_t count = 0; count < kOrders; ++count) {
    model::CashSnapshot before_cash;
    model::PositionSnapshot before_position;
    if (!read_only.trading().TryGetCash(before_cash) ||
        !read_only.trading().TryGetPosition(instrument, before_position)) {
      return "account query failed before request";
    }
    model::ClientOrderId id;
    trading.BeginCallback(CallbackPhase::kTick);
    const auto submitted = context.trading().SubmitOrder(
        {instrument, sell ? model::OrderSide::kSell : model::OrderSide::kBuy,
         1001, 201},
        id);
    trading.EndCallback();
    if (submitted != base::ErrorCode::kOk || !id.value) {
      return "request was not accepted locally";
    }
    model::CashSnapshot reserved_cash;
    model::PositionSnapshot reserved_position;
    if (!read_only.trading().TryGetCash(reserved_cash) ||
        !read_only.trading().TryGetPosition(instrument, reserved_position)) {
      return "reserved account query failed";
    }
    if (sell) {
      if (reserved_cash != before_cash ||
          reserved_position.available_qty !=
              before_position.available_qty - 201 ||
          reserved_position.reserved_qty !=
              before_position.reserved_qty + 201) {
        return "sell reservation was not visible immediately";
      }
    } else if (reserved_position != before_position ||
               reserved_cash.balance != before_cash.balance ||
               reserved_cash.reserved != before_cash.reserved + kReservation ||
               reserved_cash.available !=
                   before_cash.available - kReservation) {
      return "buy reservation was not visible immediately";
    }
    if (!trading.ProcessRequests(adapter, clock.Now()).ok()) {
      return "accepted request was not sent";
    }
    bool finished = false;
    for (std::size_t step = 0; step < 6; ++step) {
      const auto deadline = adapter.next_deadline();
      if (!deadline || *deadline <= clock.Now()) {
        return "mock did not schedule a future event";
      }
      model::OrderSnapshot before;
      if (!context.trading().TryGetOrder(id, before)) {
        return "pending order was not queryable";
      }
      TradingEvent event;
      event.report_sequence = 999;
      clock.now = *deadline - 1;
      const auto idle = adapter.Poll(clock.Now(), event);
      if (idle.state != TradingPollState::kIdle || !idle.status.ok() ||
          event.report_sequence != 999) {
        return "idle poll modified its output";
      }
      clock.now = *deadline;
      const auto polled = adapter.Poll(clock.Now(), event);
      if (polled.state != TradingPollState::kEvent || !polled.status.ok()) {
        return "scheduled broker event was not produced";
      }
      TradingDispatch dispatch;
      if (trading.ApplyEvent(event, dispatch) != base::ErrorCode::kOk ||
          !dispatch.notify_order) {
        return "valid broker event was not committed";
      }
      trading.BeginCallback(CallbackPhase::kOrderUpdate);
      model::CashSnapshot committed_cash;
      model::PositionSnapshot committed_position;
      if (!read_only.trading().TryGetCash(committed_cash) ||
          !read_only.trading().TryGetPosition(instrument, committed_position)) {
        return "account queries failed in update callback";
      }
      model::OrderSnapshot snapshot;
      if (!context.trading().TryGetOrder(id, snapshot) ||
          snapshot != dispatch.update.order) {
        return "callback query differs from committed event";
      }
      if (cancel && snapshot.state == model::OrderState::kWorking &&
          !snapshot.cancel_pending) {
        if (context.trading().CancelOrder(id) != base::ErrorCode::kOk) {
          return "working order cancel was not accepted";
        }
      }
      trading.EndCallback();
      if (dispatch.has_fill) {
        trading.BeginCallback(CallbackPhase::kFill);
        if (!read_only.trading().TryGetOrder(id, snapshot) ||
            snapshot.cumulative_filled_qty !=
                before.cumulative_filled_qty + dispatch.fill.quantity) {
          return "fill callback observed the wrong cumulative quantity";
        }
        trading.EndCallback();
        ++fill_count;
        filled_qty += dispatch.fill.quantity;
      }
      TradingDispatch duplicate;
      if (trading.ApplyEvent(event, duplicate) != base::ErrorCode::kOk ||
          duplicate.notify_order || duplicate.has_fill) {
        return "duplicate report was dispatched twice";
      }
      model::CashSnapshot duplicate_cash;
      model::PositionSnapshot duplicate_position;
      if (!read_only.trading().TryGetCash(duplicate_cash) ||
          !read_only.trading().TryGetPosition(instrument, duplicate_position) ||
          duplicate_cash != committed_cash ||
          duplicate_position != committed_position) {
        return "duplicate report changed account balances";
      }
      if (!trading.ProcessRequests(adapter, clock.Now()).ok()) {
        return "accepted cancel was not sent";
      }
      if (!context.trading().TryGetOrder(id, snapshot)) {
        return "retained order query failed";
      }
      if (model::IsTerminalOrderState(snapshot.state) &&
          !snapshot.cancel_pending) {
        if ((cancel && snapshot.state != model::OrderState::kCanceled) ||
            (!cancel && (snapshot.state != model::OrderState::kFilled ||
                         snapshot.cumulative_filled_qty != 201))) {
          return "order ended in the wrong state";
        }
        finished = true;
        break;
      }
    }
    if (!finished) {
      return "order did not terminate in the bounded script";
    }
    model::CashSnapshot final_cash;
    model::PositionSnapshot final_position;
    if (!read_only.trading().TryGetCash(final_cash) ||
        !read_only.trading().TryGetPosition(instrument, final_position) ||
        final_cash.balance !=
            initial_cash.balance + (sell ? 1 : -1) * filled_qty * kUnitCash ||
        final_cash.reserved != 0 ||
        final_cash.available != final_cash.balance ||
        final_position.total_qty !=
            initial_position.total_qty + (sell ? -1 : 1) * filled_qty ||
        final_position.available_qty !=
            initial_position.available_qty - (sell ? filled_qty : 0) ||
        final_position.reserved_qty != 0) {
      return "terminal account did not release or settle its reservation";
    }
  }
  trading.BeginStopping();
  if (!trading.ScheduleStopping(clock.Now()).ok() || !trading.is_drained() ||
      !adapter.is_drained()) {
    return "completed orders retained outstanding work";
  }
  return nullptr;
}

// Configures resources before counting, including the cold first request and
// first Poll inside the observation. Covers both fills and cancel outcomes.
void CheckTradingHeap(bool cancel, bool sell = false) {
  TradingHeapClock clock;
  MarketDataConfig market_config;
  market_config.source_id = {1};
  market_config.interval_ns = 1000000;
  market_config.instrument_count = 1;
  auto& item = market_config.instruments[0];
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
            base::ErrorCode::kOk);
  item.instrument.venue = model::Venue::kSse;
  std::copy_n("600000", 6, item.instrument.symbol);
  item.instrument.price_decimals = 2;
  item.bid_price = 1000;
  item.ask_price = 1001;
  item.bid_qty = 100;
  item.ask_qty = 200;
  MarketDataImpl market_data(clock);
  ASSERT_TRUE(market_data.Configure(market_config).ok());
  TradingImpl trading(clock, market_data);
  TradingAccountSnapshot account;
  account.cash_balance = 1000000000000000;
  if (sell) {
    account.position_count = 1;
    account.positions[0] = {item.instrument.id, 128 * 201, 128 * 201, 0};
  }
  ASSERT_TRUE(trading.Configure(account).ok());
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  TradingConfig config;
  config.mode = cancel ? MockTradingMode::kWorking : MockTradingMode::kFill;
  config.initial_account = account;
  MockTradingAdapter adapter;
  ASSERT_TRUE(adapter.Prepare(config).ok());
  ASSERT_TRUE(adapter.Start(clock.Now()).ok());
  trading.SetRunning(true);
  std::size_t fills = 0;
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure = ExerciseOrders(trading, adapter, context, clock,
                             item.instrument.id, cancel, sell, fills);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(fills, cancel ? 0U : 256U);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
  EXPECT_TRUE(adapter.Stop().ok());
}

TEST(TradingCppHeapTest, FirstRequestAndRepeatedFillsHaveNoCppHeapActivity) {
  CheckTradingHeap(false);
}

TEST(TradingCppHeapTest, RepeatedCancelsHaveNoCppHeapActivity) {
  CheckTradingHeap(true);
}

TEST(TradingCppHeapTest, SellReservationsFillsAndQueriesHaveNoCppHeapActivity) {
  CheckTradingHeap(false, true);
}

TEST(TradingCppHeapTest, SellCancelReleasesHaveNoCppHeapActivity) {
  CheckTradingHeap(true, true);
}

// Drains one real protocol path, including callback-side account queries and
// duplicate reports. All reporting stays outside the allocation window.
const char* ExerciseFuturesOrder(TradingImpl& trading,
                                 SimulatedFuturesExchange& exchange,
                                 TradingHeapClock& clock,
                                 const model::OrderRequest& request,
                                 bool cancel, std::size_t& fills) noexcept {
  model::ClientOrderId id;
  trading.BeginCallback(CallbackPhase::kTick);
  const auto submitted = trading.SubmitOrder(request, id);
  trading.EndCallback();
  if (submitted != base::ErrorCode::kOk ||
      !trading.ProcessRequests(exchange, clock.Now()).ok())
    return "futures request failed";
  for (std::size_t step = 0; step != 8; ++step) {
    const auto deadline = exchange.next_deadline();
    if (!deadline) return "futures exchange lost a pending event";
    clock.now = std::max(clock.now, *deadline);
    TradingEvent event;
    const auto polled = exchange.Poll(clock.Now(), event);
    if (!polled.status.ok() || polled.state != TradingPollState::kEvent)
      return "futures exchange failed to publish a due event";
    TradingDispatch dispatch;
    if (trading.ApplyEvent(event, dispatch) != base::ErrorCode::kOk)
      return "futures ledger rejected a valid report";
    trading.BeginCallback(CallbackPhase::kOrderUpdate);
    model::OrderSnapshot order;
    model::FuturesAccountSnapshot account;
    model::FuturesPositionSnapshot position;
    if (!trading.TryGetOrder(id, order) ||
        !trading.TryGetFuturesAccount(account) ||
        !trading.TryGetFuturesPosition(request.instrument_id, position) ||
        order != dispatch.update.order)
      return "futures callback query differs from committed facts";
    if (cancel && order.state == model::OrderState::kWorking &&
        !order.cancel_pending &&
        trading.CancelOrder(id) != base::ErrorCode::kOk)
      return "futures cancel request failed";
    trading.EndCallback();
    if (dispatch.has_fill) {
      trading.BeginCallback(CallbackPhase::kFill);
      const bool queried =
          trading.TryGetFuturesAccount(account) &&
          trading.TryGetFuturesPosition(request.instrument_id, position);
      trading.EndCallback();
      if (!queried || dispatch.fill.position_effect != request.position_effect)
        return "futures fill did not preserve its effect or account";
      ++fills;
    }
    TradingDispatch duplicate;
    if (trading.ApplyEvent(event, duplicate) != base::ErrorCode::kOk ||
        duplicate.has_fill || duplicate.notify_order)
      return "duplicate futures report was applied twice";
    if (!trading.ProcessRequests(exchange, clock.Now()).ok() ||
        !trading.TryGetOrder(id, order))
      return "futures post-callback request or query failed";
    if (model::IsTerminalOrderState(order.state) && !order.cancel_pending) {
      if (order.state !=
          (cancel ? model::OrderState::kCanceled : model::OrderState::kFilled))
        return "futures order terminated in the wrong state";
      return nullptr;
    }
  }
  return "futures order exceeded the bounded protocol script";
}

// Repeatedly opens, revalues, closes through FIFO and cancels another opening
// order. The real exchange matches quotes and returns acknowledgments/reports;
// the real order ledger maintains reservations and normalized execution state.
const char* ExerciseFutures(TradingImpl& trading,
                            SimulatedFuturesExchange& exchange,
                            TradingHeapClock& clock, model::InstrumentId id,
                            std::size_t& fills) noexcept {
  model::QuoteTick quote;
  quote.source_id = {1};
  quote.instrument_id = id;
  quote.bid_qty = quote.ask_qty = 10;
  quote.flags = static_cast<std::uint8_t>(model::QuoteFlag::kBidValid) |
                static_cast<std::uint8_t>(model::QuoteFlag::kAskValid) |
                static_cast<std::uint8_t>(model::QuoteFlag::kTradable);
  for (std::size_t round = 0; round != 128; ++round) {
    quote.sequence = 2 * round + 1;
    quote.bid_price = 100;
    quote.ask_price = 101;
    quote.receive_time_ns = clock.Now();
    if (trading.ApplyQuote(quote) != base::ErrorCode::kOk ||
        !exchange.OnQuote(quote, clock.Now()).ok())
      return "opening quote failed";
    const auto opened = ExerciseFuturesOrder(
        trading, exchange, clock,
        {id, model::OrderSide::kBuy, 101, 2, model::PositionEffect::kOpen},
        false, fills);
    if (opened != nullptr) return opened;
    quote.sequence = 2 * round + 2;
    quote.bid_price = 102;
    quote.ask_price = 103;
    quote.receive_time_ns = clock.Now();
    if (trading.ApplyQuote(quote) != base::ErrorCode::kOk ||
        !exchange.OnQuote(quote, clock.Now()).ok())
      return "closing mark/quote failed";
    const auto closed =
        ExerciseFuturesOrder(trading, exchange, clock,
                             {id, model::OrderSide::kSell, 102, 2,
                              model::PositionEffect::kCloseToday},
                             false, fills);
    if (closed != nullptr) return closed;
    const auto canceled = ExerciseFuturesOrder(
        trading, exchange, clock,
        {id, model::OrderSide::kBuy, 100, 1, model::PositionEffect::kOpen},
        true, fills);
    if (canceled != nullptr) return canceled;
    model::FuturesAccountSnapshot account;
    model::FuturesPositionSnapshot position;
    if (!trading.TryGetFuturesAccount(account) ||
        !trading.TryGetFuturesPosition(id, position) || account.reserved != 0 ||
        account.margin != 0 || account.unrealized_pnl != 0 ||
        position.long_today.quantity != 0 ||
        position.short_today.quantity != 0 ||
        account.realized_pnl !=
            static_cast<std::int64_t>(round + 1) * 20000000 ||
        account.commission != static_cast<std::int64_t>(round + 1) * 10000000)
      return "futures round did not settle FIFO, margin and fees";
  }
  return trading.is_drained() && exchange.is_drained()
             ? nullptr
             : "futures rounds retained outstanding work";
}

TEST(TradingCppHeapTest, FuturesFifoAndExchangeHotPathsHaveNoCppHeapActivity) {
  TradingHeapClock clock;
  MarketDataConfig market_config;
  market_config.source_id = {1};
  market_config.interval_ns = 1000000;
  market_config.instrument_count = 1;
  auto& item = market_config.instruments[0];
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", item.instrument.id),
            base::ErrorCode::kOk);
  item.instrument.venue = model::Venue::kShfe;
  std::copy_n("rb2701", 6, item.instrument.symbol);
  item.instrument.type = model::InstrumentType::kFutures;
  item.instrument.contract_multiplier = 10;
  item.instrument.margin_rate_ppm = 100000;
  item.instrument.open_fee_per_lot = 2000000;
  item.instrument.close_today_fee_per_lot = 3000000;
  item.instrument.close_yesterday_fee_per_lot = 1000000;
  item.bid_price = 100;
  item.ask_price = 101;
  item.bid_qty = item.ask_qty = 10;
  MarketDataImpl market(clock);
  ASSERT_TRUE(market.Configure(market_config).ok());
  TradingAccountSnapshot account;
  account.type = model::AccountType::kFutures;
  account.cash_balance = 1000000000000;
  TradingImpl trading(clock, market);
  ASSERT_TRUE(trading.Configure(account).ok());
  TradingConfig config;
  config.adapter = TradingAdapterType::kSimulatedFutures;
  config.initial_account = account;
  SimulatedFuturesExchange exchange;
  ASSERT_TRUE(exchange.Prepare(config).ok());
  ASSERT_TRUE(exchange.Start(clock.Now()).ok());
  trading.SetRunning(true);
  const char* failure = nullptr;
  std::size_t fills = 0;
  const auto activity = ObserveCppHeap([&]() noexcept {
    failure =
        ExerciseFutures(trading, exchange, clock, item.instrument.id, fills);
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(fills, 256);
  EXPECT_EQ(activity.allocation_calls, 0);
  EXPECT_EQ(activity.deallocation_calls, 0);
  EXPECT_TRUE(exchange.Stop().ok());
}

TEST(TimerCppHeapTest,
     FirstUseAndRepeatedOneShotOperationsHaveNoCppHeapActivity) {
  const char* failure = nullptr;
  const auto activity = ObserveCppHeap([&]() noexcept {
    TimerImpl timer;
    timer.BeginCallback(CallbackPhase::kStart);
    if (timer.Arm(0) != base::ErrorCode::kOk) {
      failure = "startup timer could not be armed";
      return;
    }
    timer.EndCallback();
    timer.SetRunning(true);
    const strategy::Timer& read_only = timer;
    for (std::uint64_t now = 0; now < 4096; ++now) {
      strategy::TimerEvent event;
      if (read_only.deadline() != now || !timer.TryTakeDue(now, event) ||
          event.deadline_ns != now || event.fire_time_ns != now ||
          read_only.deadline()) {
        failure = "one-shot dispatch or copied query was inconsistent";
        return;
      }
      timer.BeginCallback(CallbackPhase::kTimer);
      if (timer.Arm(now + 2) != base::ErrorCode::kOk ||
          timer.Cancel() != base::ErrorCode::kOk || read_only.deadline() ||
          timer.Arm(now + 1) != base::ErrorCode::kOk) {
        failure = "callback replacement, cancellation or rearming failed";
        return;
      }
      timer.EndCallback();
    }
    timer.BeginStopping();
    if (read_only.deadline()) failure = "stopping retained a timer";
  });
  EXPECT_EQ(failure, nullptr) << (failure == nullptr ? "" : failure);
  EXPECT_EQ(activity.allocation_calls, 0U);
  EXPECT_EQ(activity.deallocation_calls, 0U);
}

}  // namespace
}  // namespace yquant::strategyd
