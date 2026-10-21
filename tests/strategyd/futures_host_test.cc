#include <cstdint>
#include <optional>
#include <string>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/mock_trading_adapter.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"

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
  StrategyRunner runner(&clock);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_QUOTE_EXAMPLE_PATH,
                                       YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH,
                                       market.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run(environment).ok()) << runner.error_detail();
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(environment.waits, 0);
  const auto output = captured.Read();
  EXPECT_NE(output.find("quote_observer: ticks=2 last_sequence=2 "),
            std::string::npos);
}

// A wrapping adapter confirms injection remains authoritative and that a
// quote-side transport failure is observed before any strategy tick callback.
class QuoteFailureAdapter final : public TradingAdapter {
 public:
  int prepares = 0;
  int quote_calls = 0;
  int stops = 0;
  base::Status Prepare(const TradingConfig& config) noexcept override {
    ++prepares;
    return mock_.Prepare(config);
  }
  base::Status Start(std::uint64_t now) noexcept override {
    return mock_.Start(now);
  }
  base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override {
    return mock_.ReadAccountSnapshot(out);
  }
  base::Status OnQuote(const model::QuoteTick&,
                       std::uint64_t) noexcept override {
    ++quote_calls;
    return {base::ErrorCode::kIoError, {}};
  }
  base::Status SendOrder(model::ClientOrderId id,
                         const model::OrderRequest& request,
                         std::uint64_t now) noexcept override {
    return mock_.SendOrder(id, request, now);
  }
  base::Status SendCancel(model::ClientOrderId id,
                          const model::BrokerOrderId& broker,
                          model::CancelRequestId cancel,
                          std::uint64_t now) noexcept override {
    return mock_.SendCancel(id, broker, cancel, now);
  }
  TradingPollResult Poll(std::uint64_t now,
                         TradingEvent& out) noexcept override {
    return mock_.Poll(now, out);
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return mock_.next_deadline();
  }
  bool is_drained() const noexcept override { return mock_.is_drained(); }
  base::Status Stop() noexcept override {
    ++stops;
    return mock_.Stop();
  }
  const char* error_detail() const noexcept override {
    return "injected OnQuote failure";
  }

 private:
  MockTradingAdapter mock_;
};

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
  QuoteFailureAdapter adapter;
  StrategyRunner runner(&clock, nullptr, nullptr, &adapter);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_QUOTE_EXAMPLE_PATH,
                                       YQUANT_TEST_QUOTE_EXAMPLE_CONFIG_PATH,
                                       market.path(), trading.path()})
                  .ok())
      << runner.error_detail();
  EXPECT_EQ(runner.Run(environment).code, base::ErrorCode::kIoError);
  EXPECT_EQ(adapter.prepares, 1);
  EXPECT_EQ(adapter.quote_calls, 1);
  EXPECT_EQ(adapter.stops, 1);
  EXPECT_STREQ(runner.error_stage(), "trading_quote");
  EXPECT_NE(std::string(runner.error_detail()).find("injected OnQuote failure"),
            std::string::npos);
  const auto output = captured.Read();
  EXPECT_EQ(output.find("first quote received"), std::string::npos);
  EXPECT_NE(output.find("quote_observer: ticks=0 last_sequence=0 "),
            std::string::npos);
}

}  // namespace
}  // namespace yquant::strategyd
