#include "strategyd/timer.h"

#include <array>
#include <cstdint>
#include <limits>

#include "gtest/gtest.h"
#include "mock_trading_service.h"
#include "strategyd/strategy_context.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;
using strategy::TimerEvent;

TEST(TimerTest, StartupArmingReplacesTheDeadlineButCannotDispatch) {
  TimerImpl timer;
  TimerEvent event{7, 8};
  EXPECT_FALSE(timer.deadline());
  EXPECT_EQ(timer.Arm(10), ErrorCode::kInvalidState);
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(timer.Arm(10), ErrorCode::kOk);
  ASSERT_EQ(timer.Arm(20), ErrorCode::kOk);
  EXPECT_FALSE(timer.TryTakeDue(100, event));
  EXPECT_EQ(event.deadline_ns, 7U);
  EXPECT_EQ(event.fire_time_ns, 8U);
  timer.EndCallback();
  EXPECT_FALSE(timer.TryTakeDue(100, event));
  ASSERT_EQ(timer.deadline(), 20U);
  timer.SetRunning(true);
  EXPECT_FALSE(timer.TryTakeDue(19, event));
  EXPECT_EQ(event.deadline_ns, 7U);
  EXPECT_EQ(timer.deadline(), 20U);
  ASSERT_TRUE(timer.TryTakeDue(23, event));
  EXPECT_EQ(event.deadline_ns, 20U);
  EXPECT_EQ(event.fire_time_ns, 23U);
  EXPECT_FALSE(timer.deadline());
  EXPECT_FALSE(timer.TryTakeDue(24, event));
  EXPECT_EQ(event.fire_time_ns, 23U);
}

TEST(TimerTest, CancelIsIdempotentAndReplacementHasNoStaleNotification) {
  TimerImpl timer;
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(timer.Cancel(), ErrorCode::kOk);
  ASSERT_EQ(timer.Arm(10), ErrorCode::kOk);
  ASSERT_EQ(timer.Arm(30), ErrorCode::kOk);
  timer.EndCallback();
  timer.SetRunning(true);
  TimerEvent event{1, 2};
  EXPECT_FALSE(timer.TryTakeDue(10, event));
  EXPECT_EQ(event.deadline_ns, 1U);
  timer.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(timer.Cancel(), ErrorCode::kOk);
  ASSERT_EQ(timer.Cancel(), ErrorCode::kOk);
  timer.EndCallback();
  EXPECT_FALSE(timer.TryTakeDue(100, event));
  EXPECT_EQ(event.fire_time_ns, 2U);
}

TEST(TimerTest, ImmediateAndMaximumDeadlinesUseTheExactMonotonicValue) {
  constexpr auto kMaximum = std::numeric_limits<std::uint64_t>::max();
  TimerImpl timer;
  timer.SetRunning(true);
  TimerEvent event;
  for (const std::uint64_t deadline :
       {std::uint64_t{0}, std::uint64_t{5}, kMaximum}) {
    timer.BeginCallback(strategy::CallbackPhase::kTick);
    ASSERT_EQ(timer.Arm(deadline), ErrorCode::kOk);
    timer.EndCallback();
    ASSERT_TRUE(timer.TryTakeDue(kMaximum, event));
    EXPECT_EQ(event.deadline_ns, deadline);
    EXPECT_EQ(event.fire_time_ns, kMaximum);
    EXPECT_FALSE(timer.deadline());
  }
}

TEST(TimerTest, RearmingInsideCallbackNeverConsumesRecursively) {
  TimerImpl timer;
  timer.SetRunning(true);
  timer.BeginCallback(strategy::CallbackPhase::kTick);
  ASSERT_EQ(timer.Arm(0), ErrorCode::kOk);
  TimerEvent event{1, 2};
  EXPECT_FALSE(timer.TryTakeDue(10, event));
  EXPECT_EQ(event.deadline_ns, 1U);
  timer.EndCallback();
  ASSERT_TRUE(timer.TryTakeDue(10, event));
  EXPECT_FALSE(timer.deadline());
  timer.BeginCallback(strategy::CallbackPhase::kTimer);
  ASSERT_EQ(timer.Arm(0), ErrorCode::kOk);
  EXPECT_FALSE(timer.TryTakeDue(11, event));
  EXPECT_EQ(event.fire_time_ns, 10U);
  EXPECT_EQ(timer.deadline(), 0U);
  timer.EndCallback();
  ASSERT_TRUE(timer.TryTakeDue(12, event));
  EXPECT_EQ(event.deadline_ns, 0U);
  EXPECT_EQ(event.fire_time_ns, 12U);
}

TEST(TimerTest, MutationPhaseFailuresPreserveAnArmedDeadline) {
  constexpr std::array kBusinessPhases = {
      strategy::CallbackPhase::kTick, strategy::CallbackPhase::kOrderUpdate,
      strategy::CallbackPhase::kFill, strategy::CallbackPhase::kTimer};
  TimerImpl timer;
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(timer.Arm(100), ErrorCode::kOk);
  timer.EndCallback();
  for (const auto phase : kBusinessPhases) {
    timer.BeginCallback(phase);
    EXPECT_EQ(timer.Arm(200), ErrorCode::kInvalidState);
    EXPECT_EQ(timer.Cancel(), ErrorCode::kInvalidState);
    EXPECT_EQ(timer.deadline(), 100U);
    timer.EndCallback();
  }
  timer.SetRunning(true);
  for (const auto phase : kBusinessPhases) {
    timer.BeginCallback(phase);
    ASSERT_EQ(timer.Cancel(), ErrorCode::kOk);
    ASSERT_EQ(timer.Arm(100), ErrorCode::kOk);
    timer.EndCallback();
  }
  for (const auto phase :
       {strategy::CallbackPhase::kNone, strategy::CallbackPhase::kStop}) {
    timer.BeginCallback(phase);
    EXPECT_EQ(timer.Arm(200), ErrorCode::kInvalidState);
    EXPECT_EQ(timer.Cancel(), ErrorCode::kInvalidState);
    EXPECT_EQ(timer.deadline(), 100U);
    timer.EndCallback();
  }
}

TEST(TimerTest, StoppingDiscardsTheDeadlineAndResetStartsAFreshLifecycle) {
  TimerImpl timer;
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(timer.Arm(0), ErrorCode::kOk);
  timer.EndCallback();
  timer.SetRunning(true);
  timer.BeginStopping();
  timer.BeginStopping();
  EXPECT_FALSE(timer.deadline());
  for (const auto phase :
       {strategy::CallbackPhase::kStart, strategy::CallbackPhase::kTimer,
        strategy::CallbackPhase::kStop}) {
    timer.BeginCallback(phase);
    EXPECT_EQ(timer.Arm(0), ErrorCode::kInvalidState);
    EXPECT_EQ(timer.Cancel(), ErrorCode::kInvalidState);
    timer.EndCallback();
  }
  TimerEvent event{1, 2};
  EXPECT_FALSE(timer.TryTakeDue(100, event));
  EXPECT_EQ(event.deadline_ns, 1U);
  timer.Reset();
  EXPECT_FALSE(timer.deadline());
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(timer.Arm(10), ErrorCode::kOk);
  timer.EndCallback();
  EXPECT_FALSE(timer.TryTakeDue(100, event));
  timer.SetRunning(true);
  ASSERT_TRUE(timer.TryTakeDue(100, event));
  EXPECT_EQ(event.deadline_ns, 10U);
}

TEST(TimerTest, ContextReturnsTheSameOwnedTimerThroughBothAccessors) {
  base::SteadyMonotonicClock clock;
  MarketDataImpl market_data(clock);
  testing::MockTradingService trading;
  TimerImpl timer;
  StrategyContextImpl context(clock, market_data, trading, timer);
  const strategy::StrategyContext& read_only = context;
  EXPECT_EQ(&context.timer(), &read_only.timer());
  timer.BeginCallback(strategy::CallbackPhase::kStart);
  ASSERT_EQ(context.timer().Arm(1234), ErrorCode::kOk);
  EXPECT_EQ(read_only.timer().deadline(), 1234U);
  timer.EndCallback();
}

}  // namespace
}  // namespace yquant::strategyd
