#include <cstdint>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::strategyd {
namespace {

// The real replay source is selected from JSON, including its zero-tail EOF.
// Equity observation isolates host callback/EOF ordering from order behavior.
std::string ObserverReplayJson(const char* path) {
  return R"({"adapter":"replay","source_id":1,"replay_tail_ms":0,)"
         R"("replay_path":")" +
         std::string(path) + R"(","instrument_catalog":")" +
         std::string(YQUANT_TEST_INSTRUMENT_CATALOG_PATH) +
         R"(","instruments":[{"venue":"SSE","symbol":"600000"}]})";
}

TEST(FuturesHostTest, ZeroReplayTailDoesNotSkipTheLastOfTwoDueQuotes) {
  testing::ConfigFile csv(
      "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n"
      "0,0,1000,100,1001,200,7\n0,0,1001,100,1002,200,7\n");
  ASSERT_TRUE(csv.valid());
  testing::ConfigFile market(ObserverReplayJson(csv.path()));
  ASSERT_TRUE(market.valid());
  loop_testing::StderrCapture captured;
  ASSERT_TRUE(captured.valid());
  loop_testing::Clock clock;
  loop_testing::Environment environment(clock);
  ReplayMarketDataSource source;
  testing::MockTradingService service;
  RunnerTestHarness runner(clock, environment, source, service);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_QUOTE_EXAMPLE_PATH,
                      YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH, market.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(environment.waits, 0);
  const auto output = captured.Read();
  EXPECT_NE(output.find("quote_observer: ticks=2 last_sequence=2 "),
            std::string::npos);
}

TEST(FuturesHostTest,
     AdapterQuoteFailurePrecedesOnTickAndPreservesItsDiagnostic) {
  testing::ConfigFile csv(
      "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n"
      "0,0,1000,100,1001,200,7\n");
  ASSERT_TRUE(csv.valid());
  testing::ConfigFile market(ObserverReplayJson(csv.path()));
  testing::ConfigFile trading(loop_testing::TradingJson());
  ASSERT_TRUE(market.valid());
  ASSERT_TRUE(trading.valid());
  loop_testing::StderrCapture captured;
  ASSERT_TRUE(captured.valid());
  loop_testing::Clock clock;
  loop_testing::Environment environment(clock);
  ReplayMarketDataSource source;
  testing::MockTradingService service;
  service.quote_result = {base::ErrorCode::kIoError, {}};
  RunnerTestHarness runner(clock, environment, source, service);
  ASSERT_TRUE(runner
                  .LoadAndStart(StrategyOptions{
                      YQUANT_TEST_QUOTE_EXAMPLE_PATH,
                      YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH, market.path()})
                  .ok())
      << runner.error_detail();
  EXPECT_EQ(runner.Run().code, base::ErrorCode::kIoError);
  EXPECT_EQ(service.init_calls, 1);
  EXPECT_EQ(service.quote_calls, 1);
  EXPECT_EQ(service.stop_calls, 1);
  EXPECT_STREQ(runner.error_stage(), "trading_quote");
  EXPECT_NE(std::string(runner.error_detail())
                .find("injected trading service failure"),
            std::string::npos);
  const auto output = captured.Read();
  EXPECT_EQ(output.find("first quote received"), std::string::npos);
  EXPECT_NE(output.find("quote_observer: ticks=0 last_sequence=0 "),
            std::string::npos);
}

}  // namespace
}  // namespace yquant::strategyd
