#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"

namespace yquant::strategyd {
namespace {

using loop_testing::Broker;
using loop_testing::Clock;
using loop_testing::Environment;
using loop_testing::kMs;
using loop_testing::QuoteSource;
using loop_testing::StderrCapture;
using loop_testing::SummaryNumber;
using loop_testing::TradingJson;
using model::OrderSide;

std::string BboJson(std::int64_t minimum = 0, std::int64_t maximum = 1200,
                    std::int64_t quantity = 100, std::uint64_t timeout_ms = 100,
                    std::uint64_t retry_ms = 5) {
  return R"({"strategy_id":"bbo-test","parameters":{"source_id":1,"venue":"SSE",)"
         R"("symbol":"600000","quote_qty":)" +
         std::to_string(quantity) + R"(,"min_position":)" +
         std::to_string(minimum) + R"(,"max_position":)" +
         std::to_string(maximum) + R"(,"quote_timeout_ms":)" +
         std::to_string(timeout_ms) + R"(,"retry_delay_ms":)" +
         std::to_string(retry_ms) + "}}";
}

std::string BboUnknownFieldJson() {
  auto json = BboJson();
  json.insert(json.size() - 2, R"(,"unknown":true)");
  return json;
}

std::string BboStringQuantityJson() {
  auto json = BboJson();
  constexpr std::string_view kQuantity = R"("quote_qty":100)";
  json.replace(json.find(kQuantity), kQuantity.size(), R"("quote_qty":"100")");
  return json;
}

// Drives the actual BBO module using copied timed quotes and broker facts.
// Captures only the final low-frequency summary; assertions inspect sends.
class BboScenario {
 public:
  BboScenario() : environment(clock) { environment.stop_at = 3 * kMs; }
  base::Status Run(std::string_view config = {},
                   std::int64_t cash = 100000000000000,
                   std::int64_t total = 1000, std::int64_t available = 800) {
    testing::ConfigFile strategy(config.empty() ? BboJson()
                                                : std::string(config));
    testing::ConfigFile trading(TradingJson(cash, total, available));
    if (!strategy.valid() || !trading.valid())
      return {base::ErrorCode::kIoError, {}};
    StrategyRunner runner(&clock, &source, nullptr, &broker);
    auto result = runner.Handle(
        LoadAndStart{YQUANT_TEST_BBO_EXAMPLE_PATH, strategy.path(),
                     YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()});
    if (result.ok()) result = runner.Run(environment);
    stage = runner.error_stage();
    detail = runner.error_detail();
    output = capture.Read();
    return result;
  }
  Clock clock;
  QuoteSource source;
  Broker broker;
  Environment environment;
  StderrCapture capture;
  std::string stage;
  std::string detail;
  std::string output;
};

std::size_t CountSide(const Broker& broker, OrderSide side) {
  std::size_t count = 0;
  for (std::size_t index = 0; index < broker.order_count; ++index) {
    if (broker.orders[index].request.side == side) ++count;
  }
  return count;
}

TEST(BboStrategyTest, PostsBothSidesAndRetainsOrdersForRepeatedPrices) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1000, 1001};
  scenario.source.frame_count = 2;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 2U);
  EXPECT_EQ(scenario.broker.orders[0].request.side, OrderSide::kBuy);
  EXPECT_EQ(scenario.broker.orders[0].request.limit_price, 1000);
  EXPECT_EQ(scenario.broker.orders[1].request.side, OrderSide::kSell);
  EXPECT_EQ(scenario.broker.orders[1].request.limit_price, 1001);
  EXPECT_EQ(scenario.broker.orders[0].request.quantity, 100);
  EXPECT_EQ(scenario.broker.orders[1].request.quantity, 100);
  ASSERT_EQ(scenario.broker.cancel_count, 2U);
  for (std::size_t index = 0; index < 2; ++index)
    EXPECT_GE(scenario.broker.cancels[index].at, 3 * kMs);
  EXPECT_EQ(SummaryNumber(scenario.output, "submitted="), 2);
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
  EXPECT_TRUE(scenario.broker.is_drained());
}

TEST(BboStrategyTest,
     WaitsForCancellationAndReplacesUsingTheLatestOfTwoUpdates) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1002, 1003};
  scenario.source.frames[2] = {2 * kMs, 1004, 1005};
  scenario.source.frame_count = 3;
  scenario.broker.buy_cancel_delay = 5 * kMs;
  scenario.broker.sell_cancel_delay = 5 * kMs;
  scenario.environment.stop_at = 8 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 4U);
  for (std::size_t index = 2; index < 4; ++index) {
    const auto& sent = scenario.broker.orders[index];
    EXPECT_GE(sent.at, 6 * kMs);
    EXPECT_EQ(sent.request.limit_price,
              sent.request.side == OrderSide::kBuy ? 1004 : 1005);
  }
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
}

TEST(BboStrategyTest,
     PendingNewQuotesWaitForAcknowledgementBeforeCancelAndReplace) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1002, 1003};
  scenario.source.frames[2] = {2 * kMs, 1004, 1005};
  scenario.source.frame_count = 3;
  scenario.broker.ack_delay = 3 * kMs;
  scenario.environment.stop_at = 8 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 4U);
  ASSERT_GE(scenario.broker.cancel_count, 2U);
  for (std::size_t index = 0; index < scenario.broker.cancel_count; ++index) {
    EXPECT_GE(scenario.broker.cancels[index].at, 3 * kMs);
  }
  for (std::size_t index = 2; index < 4; ++index) {
    const auto& order = scenario.broker.orders[index];
    EXPECT_GE(order.at, 3 * kMs + scenario.broker.buy_cancel_delay);
    EXPECT_EQ(order.request.limit_price,
              order.request.side == OrderSide::kBuy ? 1004 : 1005);
  }
}

TEST(BboStrategyTest, ExistingQuotesIgnoreTheirOwnFullyReservedCashAndShares) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1000, 1001};
  scenario.source.frame_count = 2;
  ASSERT_TRUE(scenario.Run({}, 1000000000, 1000, 100).ok())
      << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 2U);
  ASSERT_EQ(scenario.broker.cancel_count, 2U);
  for (std::size_t index = 0; index < 2; ++index) {
    EXPECT_GE(scenario.broker.cancels[index].at, 3 * kMs);
  }
  EXPECT_EQ(SummaryNumber(scenario.output, "cancel_requests="), 0);
  EXPECT_EQ(SummaryNumber(scenario.output, "cash_balance="), 1000000000);
  EXPECT_EQ(SummaryNumber(scenario.output, "cash_reserved="), 0);
  EXPECT_EQ(SummaryNumber(scenario.output, "available_qty="), 100);
}

class InvalidQuoteStateTest : public ::testing::TestWithParam<bool> {};

TEST_P(InvalidQuoteStateTest,
       CancelsQuotesAndOnlyReopensWhenAValidQuoteArrives) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  const std::uint8_t flags = GetParam() ? 5 : 3;
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1000, 1001, flags};
  scenario.source.frames[2] = {2 * kMs, 1000, 1001, flags};
  scenario.source.frames[3] = {3 * kMs, 1002, 1003};
  scenario.source.frame_count = 4;
  scenario.environment.stop_at = 5 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 4U);
  ASSERT_GE(scenario.broker.cancel_count, 2U);
  EXPECT_EQ(scenario.broker.cancels[0].at, kMs);
  EXPECT_EQ(scenario.broker.cancels[1].at, kMs);
  for (std::size_t index = 2; index < 4; ++index) {
    const auto& order = scenario.broker.orders[index];
    EXPECT_EQ(order.at, 3 * kMs);
    EXPECT_EQ(order.request.limit_price,
              order.request.side == OrderSide::kBuy ? 1002 : 1003);
  }
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
}

INSTANTIATE_TEST_SUITE_P(QuoteValidity, InvalidQuoteStateTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "MissingAsk" : "NotTradable";
                         });

TEST(BboStrategyTest,
     PartialFillWhileCancelPendingUpdatesHoldingsAndPreservesTheSlot) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1002, 1003};
  scenario.source.frame_count = 2;
  scenario.broker.buy_cancel_delay = 3 * kMs;
  scenario.broker.sell_cancel_delay = 3 * kMs;
  scenario.broker.first_buy_fills[0] = {2 * kMs, 50};
  scenario.broker.fill_count = 1;
  scenario.environment.stop_at = 6 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.stage << ": " << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 4U);
  EXPECT_GE(scenario.broker.orders[2].at, 4 * kMs);
  EXPECT_GE(scenario.broker.orders[3].at, 4 * kMs);
  EXPECT_EQ(SummaryNumber(scenario.output, "filled_qty="), 50);
  EXPECT_EQ(SummaryNumber(scenario.output, "total_qty="), 1050);
  EXPECT_EQ(SummaryNumber(scenario.output, "available_qty="), 800);
  EXPECT_EQ(SummaryNumber(scenario.output, "cash_reserved="), 0);
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
}

struct SideGate {
  const char* name;
  std::int64_t cash;
  std::int64_t available;
  std::int64_t minimum;
  std::int64_t maximum;
  OrderSide expected;
};
class BboSideGateTest : public ::testing::TestWithParam<SideGate> {};
TEST_P(BboSideGateTest, RespectsPositionCashAndSellableInventoryIndependently) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frame_count = 1;
  const auto& settings = GetParam();
  ASSERT_TRUE(scenario
                  .Run(BboJson(settings.minimum, settings.maximum),
                       settings.cash, 1000, settings.available)
                  .ok())
      << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 1U);
  EXPECT_EQ(scenario.broker.orders[0].request.side, settings.expected);
}
INSTANTIATE_TEST_SUITE_P(
    SideLimits, BboSideGateTest,
    ::testing::Values(
        SideGate{"Cash", 0, 800, 0, 1200, OrderSide::kSell},
        SideGate{"SellableShares", 100000000000000, 0, 0, 1200,
                 OrderSide::kBuy},
        SideGate{"Maximum", 100000000000000, 800, 0, 1000, OrderSide::kSell},
        SideGate{"Minimum", 100000000000000, 800, 1000, 1200, OrderSide::kBuy}),
    [](const ::testing::TestParamInfo<SideGate>& info) {
      return info.param.name;
    });

TEST(BboStrategyTest,
     IntradayBoughtSharesDoNotBecomeSellableAndMaximumIncludesTheFill) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frame_count = 1;
  scenario.broker.first_buy_fills[0] = {2 * kMs, 100};
  scenario.broker.fill_count = 1;
  ASSERT_TRUE(scenario.Run(BboJson(0, 1100), 100000000000000, 1000, 0).ok())
      << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 1U);
  EXPECT_EQ(scenario.broker.orders[0].request.side, OrderSide::kBuy);
  EXPECT_EQ(SummaryNumber(scenario.output, "total_qty="), 1100);
  EXPECT_EQ(SummaryNumber(scenario.output, "available_qty="), 0);
  EXPECT_EQ(SummaryNumber(scenario.output, "filled_qty="), 100);
}

TEST(BboStrategyTest,
     MissingMarketEventsStillExpireQuotesAndCancelThroughTheTimer) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frame_count = 1;
  scenario.environment.stop_at = 7 * kMs;
  ASSERT_TRUE(scenario.Run(BboJson(0, 1200, 100, 5)).ok()) << scenario.detail;
  ASSERT_EQ(scenario.broker.cancel_count, 2U);
  EXPECT_EQ(scenario.broker.cancels[0].at, 5 * kMs);
  EXPECT_EQ(scenario.broker.cancels[1].at, 5 * kMs);
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
}

TEST(BboStrategyTest,
     RejectedQuoteRetriesOnTheTimerAndNewTicksDoNotBypassBackoff) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1000, 1001};
  scenario.source.frames[2] = {2 * kMs, 1000, 1001};
  scenario.source.frame_count = 3;
  scenario.broker.reject_buys = 1;
  scenario.environment.stop_at = 7 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.detail;
  ASSERT_EQ(scenario.broker.order_count, 3U);
  EXPECT_EQ(scenario.broker.orders[2].request.side, OrderSide::kBuy);
  EXPECT_EQ(scenario.broker.orders[2].at, 5 * kMs + scenario.broker.ack_delay);
}

TEST(BboStrategyTest,
     CancelRejectionPreservesWorkingExposureAndWaitsBeforeRetry) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1002, 1003};
  scenario.source.frames[2] = {2 * kMs, 1002, 1003};
  scenario.source.frame_count = 3;
  scenario.broker.reject_cancels = 1;
  scenario.environment.stop_at = 8 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.detail;
  std::array<std::uint64_t, 2> attempts = {};
  std::size_t count = 0;
  const auto first_buy = scenario.broker.orders[0].id;
  for (std::size_t index = 0; index < scenario.broker.cancel_count; ++index) {
    if (scenario.broker.cancels[index].id == first_buy &&
        count < attempts.size())
      attempts[count++] = scenario.broker.cancels[index].at;
  }
  ASSERT_EQ(count, 2U);
  EXPECT_EQ(attempts[0], kMs);
  EXPECT_EQ(attempts[1], 6 * kMs + scenario.broker.buy_cancel_delay);
}

TEST(BboStrategyTest, DoesNotCrossAnOppositeOrderStillWaitingForCancellation) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1002};
  scenario.source.frames[1] = {kMs, 1003, 1005};
  scenario.source.frame_count = 2;
  scenario.broker.sell_cancel_delay = 5 * kMs;
  scenario.environment.stop_at = 8 * kMs;
  ASSERT_TRUE(scenario.Run().ok()) << scenario.detail;
  ASSERT_EQ(CountSide(scenario.broker, OrderSide::kBuy), 2U);
  for (std::size_t index = 2; index < scenario.broker.order_count; ++index) {
    if (scenario.broker.orders[index].request.side == OrderSide::kBuy) {
      EXPECT_GE(scenario.broker.orders[index].at, 6 * kMs);
      EXPECT_EQ(scenario.broker.orders[index].request.limit_price, 1003);
    }
  }
}

TEST(BboStrategyTest,
     LateCancelRejectionForFilledOrderDoesNotClearOrDelayItsReplacement) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  scenario.source.frames[0] = {0, 1000, 1001};
  scenario.source.frames[1] = {kMs, 1002, 1003};
  scenario.source.frames[2] = {6500000, 1004, 1005};
  scenario.source.frame_count = 3;
  scenario.broker.buy_cancel_delay = 5 * kMs;
  scenario.broker.first_buy_fills[0] = {2 * kMs, 100};
  scenario.broker.fill_count = 1;
  scenario.environment.stop_at = 9 * kMs;
  ASSERT_TRUE(scenario.Run(BboJson(0, 1500)).ok()) << scenario.detail;
  std::optional<model::ClientOrderId> replacement;
  std::size_t early_buys = 0;
  for (std::size_t index = 0; index < scenario.broker.order_count; ++index) {
    const auto& order = scenario.broker.orders[index];
    if (order.request.side == OrderSide::kBuy && order.at < 6500000) {
      ++early_buys;
      if (order.at >= 2 * kMs) replacement = order.id;
    }
  }
  ASSERT_EQ(early_buys, 2U);
  ASSERT_TRUE(replacement.has_value());
  bool canceled_on_update = false;
  for (std::size_t index = 0; index < scenario.broker.cancel_count; ++index) {
    const auto& cancel = scenario.broker.cancels[index];
    if (cancel.id == *replacement && cancel.at == 6500000)
      canceled_on_update = true;
  }
  EXPECT_TRUE(canceled_on_update);
  EXPECT_EQ(SummaryNumber(scenario.output, "slots="), 0);
}

struct InvalidBboConfig {
  const char* name;
  std::string json;
};
class InvalidBboConfigTest : public ::testing::TestWithParam<InvalidBboConfig> {
};
TEST_P(InvalidBboConfigTest, RejectsSchemaBeforeOnStart) {
  BboScenario scenario;
  ASSERT_TRUE(scenario.capture.valid());
  EXPECT_FALSE(scenario.Run(GetParam().json).ok());
  EXPECT_EQ(scenario.stage, "initialize");
  EXPECT_EQ(scenario.output.find("bbo_market_making: ready"),
            std::string::npos);
  EXPECT_EQ(scenario.broker.order_count, 0U);
}
INSTANTIATE_TEST_SUITE_P(
    InvalidParameters, InvalidBboConfigTest,
    ::testing::Values(
        InvalidBboConfig{"MissingFields",
                         R"({"strategy_id":"invalid","parameters":{}})"},
        InvalidBboConfig{"ZeroQuantity", BboJson(0, 1200, 0)},
        InvalidBboConfig{"ZeroTimeout", BboJson(0, 1200, 100, 0)},
        InvalidBboConfig{"UnknownField", BboUnknownFieldJson()},
        InvalidBboConfig{"NegativeMinimum", BboJson(-1)},
        InvalidBboConfig{"StringQuantity", BboStringQuantityJson()},
        InvalidBboConfig{"ZeroRetryDelay", BboJson(0, 1200, 100, 100, 0)},
        InvalidBboConfig{"InvalidPositionRange", BboJson(1000, 1000)}),
    [](const ::testing::TestParamInfo<InvalidBboConfig>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace yquant::strategyd
