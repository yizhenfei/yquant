#include <stdint.h>

#include <algorithm>
#include <string>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"

namespace yquant::strategyd {
namespace {

using loop_testing::Broker;
using loop_testing::Clock;
using loop_testing::Environment;
using loop_testing::EventPipe;
using loop_testing::kMs;
using loop_testing::QuoteSource;
using loop_testing::TradingJson;

struct TimerCase {
  const char* name;
  const char* plugin;
  uint64_t stop_at;
  uint64_t first_wait;
  const char* trace;
};
class TimerRunnerTest : public ::testing::TestWithParam<TimerCase> {};

TEST_P(TimerRunnerTest, DispatchesOneShotTimersWithoutMarketData) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  Environment environment(clock);
  environment.stop_at = GetParam().stop_at;
  IdleTestSource source;
  Broker broker;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{GetParam().plugin,
                                                YQUANT_TEST_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), GetParam().trace);
  ASSERT_GT(environment.waits, 0);
  EXPECT_EQ(environment.timeouts[0], GetParam().first_wait);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
}

INSTANTIATE_TEST_SUITE_P(
    RealTimerPlugins, TimerRunnerTest,
    ::testing::Values(TimerCase{"IndependentDeadline",
                                YQUANT_TEST_TIMER_BASIC_PATH, 8 * kMs, 7 * kMs,
                                "CSHTD"},
                      TimerCase{"Canceled", YQUANT_TEST_TIMER_CANCEL_PATH,
                                4 * kMs, 4 * kMs, "CSTD"},
                      TimerCase{"Replaced", YQUANT_TEST_TIMER_REPLACE_PATH,
                                4 * kMs, 3 * kMs, "CSHTD"},
                      TimerCase{"Rearmed", YQUANT_TEST_TIMER_REARM_PATH,
                                4 * kMs, kMs, "CSHHHTD"},
                      TimerCase{"PastDeadline", YQUANT_TEST_TIMER_PAST_PATH,
                                kMs, kMs, "CSHTD"}),
    [](const ::testing::TestParamInfo<TimerCase>& info) {
      return info.param.name;
    });

TEST(TimerIntegrationTest, StopDisarmsTheUnconsumedTimerBeforeOnStop) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  Environment environment(clock);
  environment.stop_at = kMs;
  IdleTestSource source;
  Broker broker;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{YQUANT_TEST_TIMER_BASIC_PATH,
                                                YQUANT_TEST_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(runner.Run().ok());
  EXPECT_EQ(events.Read(), "CSTD");
}

TEST(TimerIntegrationTest, TimerCallbacksCanSubmitAndCancelThroughTheLedger) {
  testing::ConfigFile trading(TradingJson());
  ASSERT_TRUE(trading.valid());
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  QuoteSource source;
  Broker broker;
  Environment environment(clock);
  environment.stop_at = 5 * kMs;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_TIMER_TRADING_PATH, YQUANT_TEST_CONFIG_PATH,
                      YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), "CSHNAHKXcTD");
  EXPECT_EQ(broker.sent_count, 1U);
  EXPECT_EQ(broker.cancel_count, 1U);
  EXPECT_TRUE(broker.is_drained());
}

TEST(TimerIntegrationTest, CallbackFailureDisarmsAndDrainsTheWorkingOrder) {
  testing::ConfigFile trading(TradingJson());
  ASSERT_TRUE(trading.valid());
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  QuoteSource source;
  Broker broker;
  Environment environment(clock);
  environment.stop_at = 5 * kMs;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_TIMER_FAIL_PATH, YQUANT_TEST_CONFIG_PATH,
                      YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()})
                  .ok());
  EXPECT_EQ(runner.Run().code, base::ErrorCode::kInvalidArgument);
  EXPECT_STREQ(runner.error_stage(), "timer");
  EXPECT_EQ(events.Read(), "CSHNAHcTD");
  EXPECT_EQ(broker.cancel_count, 1U);
  EXPECT_TRUE(broker.is_drained());
}

TEST(TimerIntegrationTest, ImmediatelyRearmedTimerSharesThe64EventBudget) {
  testing::ConfigFile trading(TradingJson());
  ASSERT_TRUE(trading.valid());
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  QuoteSource source;
  source.frames[0] = {0, 1000, 1001};
  source.frame_count = 1;
  source.continuous = true;
  Broker broker;
  Environment environment(clock);
  environment.advance_on_check = kMs;
  environment.stop_at = 2 * kMs;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_TIMER_FAIR_PATH, YQUANT_TEST_CONFIG_PATH,
                      YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()})
                  .ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  const auto trace = events.Read();
  const auto timers = std::count(trace.begin(), trace.end(), 'H');
  const auto quotes = std::count(trace.begin(), trace.end(), 'Q');
  EXPECT_GT(timers, 0);
  EXPECT_GT(quotes, 0);
  EXPECT_EQ(timers + quotes, 64);
  EXPECT_EQ(broker.sent_count, 0U);
  EXPECT_NE(trace.find("cTD"), std::string::npos);
}

TEST(TimerIntegrationTest,
     DueTimersDoNotStarveMarketOrTradingAndCannotSplitFillCallbacks) {
  testing::ConfigFile trading(TradingJson());
  ASSERT_TRUE(trading.valid());
  EventPipe events;
  ASSERT_TRUE(events.valid());
  Clock clock;
  QuoteSource source;
  source.frames[0] = {0, 1000, 1001};
  source.frame_count = 1;
  source.continuous = true;
  Broker broker;
  broker.first_buy_fills[0] = {kMs, 1};
  broker.fill_count = 1;
  Environment environment(clock);
  environment.advance_on_check = kMs;
  environment.stop_at = 10 * kMs;
  loop_testing::LoopRunner runner(clock, environment, source, broker);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_TIMER_FAIR_PATH, YQUANT_TEST_CONFIG_PATH,
                      YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()})
                  .ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  const auto trace = events.Read();
  EXPECT_GT(std::count(trace.begin(), trace.end(), 'Q'), 3);
  EXPECT_GT(std::count(trace.begin(), trace.end(), 'H'), 3);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'A'), 1);
  EXPECT_NE(trace.find("VF"), std::string::npos);
  EXPECT_NE(trace.find("fTD"), std::string::npos);
  EXPECT_EQ(broker.sent_count, 1U);
  EXPECT_EQ(broker.fills, 1);
  EXPECT_EQ(environment.waits, 0);
}

}  // namespace
}  // namespace yquant::strategyd
