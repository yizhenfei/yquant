#include <algorithm>
#include <csignal>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "test_config_file.h"
#include "trading_process_helpers.h"

namespace yquant::strategyd {
namespace {

using trading_testing::Process;

std::vector<std::string> OrderExampleArguments(const char* strategy_config,
                                               const char* trading_config) {
  return {"--strategy",
          YQUANT_TEST_ORDER_EXAMPLE_PATH,
          "--config",
          strategy_config,
          "--market-data-config",
          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
          "--trading-config",
          trading_config};
}

std::string ProcessTradingConfig(std::string_view mode) {
  return std::string(R"({"adapter":"mock","mode":")") + std::string(mode) +
         R"(","report_interval_ms":1,"shutdown_timeout_ms":1000,)"
         R"("initial_cash":100000000000000,"initial_positions":[)"
         R"({"venue":"SSE","symbol":"600000","total_qty":1000,"available_qty":800}]})";
}

TEST(TradingProcessTest, LoadsOrderExampleAndFills200BeforeSignal) {
  Process child;
  ASSERT_EQ(
      child.Start(OrderExampleArguments(YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH,
                                        YQUANT_TEST_TRADING_CONFIG_PATH)),
      0);
  ASSERT_TRUE(child.WaitForOutput(
      "order_lifecycle: terminal state=filled cumulative=200 leaves=0\n"))
      << child.output();
  EXPECT_NE(child.output().find("order_lifecycle: ready\n"), std::string::npos);
  EXPECT_NE(child.output().find("order_lifecycle: submitted\n"),
            std::string::npos);
  EXPECT_NE(child.output().find("strategy_id=order-lifecycle-example"),
            std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account initial cash=100000000000000 reserved=0 "
          "available=100000000000000 position_total=1000 "
          "position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account fill cash=99998999000000 "
          "reserved=1001000000 "
          "available=99997998000000 position_total=1100 position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: stopped fills=2 filled_qty=200 state=filled\n"),
      std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account final cash=99997998000000 reserved=0 "
          "available=99997998000000 position_total=1200 position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
}

TEST(TradingProcessTest, QuantityOneProducesExactlyOneFill) {
  testing::ConfigFile config(
      R"({"strategy_id":"one","parameters":{"source_id":1,"venue":"SSE",)"
      R"("symbol":"600000","quantity":1}})");
  ASSERT_TRUE(config.valid());
  Process child;
  ASSERT_EQ(child.Start(OrderExampleArguments(config.path(),
                                              YQUANT_TEST_TRADING_CONFIG_PATH)),
            0);
  ASSERT_TRUE(child.WaitForOutput(
      "order_lifecycle: terminal state=filled cumulative=1 leaves=0\n"))
      << child.output();
  ASSERT_TRUE(child.Signal(SIGINT));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find(
                "order_lifecycle: stopped fills=1 filled_qty=1 state=filled\n"),
            std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account final cash=99999989990000 reserved=0 "
          "available=99999989990000 position_total=1001 position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
}

TEST(TradingProcessTest, StopsAndAutomaticallyCancelsAWorkingOrder) {
  testing::ConfigFile config(ProcessTradingConfig("working"));
  ASSERT_TRUE(config.valid());
  Process child;
  ASSERT_EQ(child.Start(OrderExampleArguments(
                YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, config.path())),
            0);
  ASSERT_TRUE(child.WaitForOutput("order_lifecycle: working\n"))
      << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: terminal state=canceled cumulative=0 leaves=0\n"),
      std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: stopped fills=0 filled_qty=0 state=canceled\n"),
      std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account final cash=100000000000000 reserved=0 "
          "available=100000000000000 position_total=1000 "
          "position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
}

TEST(TradingProcessTest, RejectedOrderDoesNotFailTheProcess) {
  testing::ConfigFile config(ProcessTradingConfig("reject"));
  ASSERT_TRUE(config.valid());
  Process child;
  ASSERT_EQ(child.Start(OrderExampleArguments(
                YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, config.path())),
            0);
  ASSERT_TRUE(child.WaitForOutput(
      "order_lifecycle: terminal state=rejected cumulative=0 leaves=0\n"))
      << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: stopped fills=0 filled_qty=0 state=rejected\n"),
      std::string::npos);
  EXPECT_NE(
      child.output().find(
          "order_lifecycle: account final cash=100000000000000 reserved=0 "
          "available=100000000000000 position_total=1000 "
          "position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
}

TEST(TradingProcessTest, OrderExampleRemainsIdleWithoutTradingConfiguration) {
  auto arguments = OrderExampleArguments(YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH,
                                         YQUANT_TEST_TRADING_CONFIG_PATH);
  arguments.resize(arguments.size() - 2);
  Process child;
  ASSERT_EQ(child.Start(arguments), 0);
  ASSERT_TRUE(child.WaitForOutput("Running")) << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find(
                "order_lifecycle: stopped fills=0 filled_qty=0 state=none\n"),
            std::string::npos);
}

TEST(TradingProcessTest, InvalidTradingConfigPrecedesDynamicLibraryLoading) {
  testing::ConfigFile config(
      R"({"adapter":"mock","mode":"unknown",)"
      R"("report_interval_ms":1,"shutdown_timeout_ms":100,)"
      R"("initial_cash":100000000000000,"initial_positions":[]})");
  ASSERT_TRUE(config.valid());
  Process child;
  ASSERT_EQ(
      child.Start({"--strategy", "/yquant-no-such-strategy.so", "--config",
                   YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                   "--market-data-config", YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                   "--trading-config", config.path()}),
      0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=trading_config"), std::string::npos);
  EXPECT_NE(child.output().find(config.path()), std::string::npos);
  EXPECT_EQ(child.output().find("stage=dlopen"), std::string::npos);
  EXPECT_TRUE(child.events().empty());
}

TEST(TradingProcessTest, InvalidTradingArgumentsReturnUsageError) {
  const std::vector<std::vector<std::string>> invalid = {
      {"--strategy", YQUANT_TEST_ORDER_EXAMPLE_PATH, "--config",
       YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, "--trading-config",
       YQUANT_TEST_TRADING_CONFIG_PATH},
      {"--strategy", YQUANT_TEST_ORDER_EXAMPLE_PATH, "--config",
       YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, "--market-data-config",
       YQUANT_TEST_MARKET_DATA_CONFIG_PATH, "--trading-config"},
      {"--strategy", YQUANT_TEST_ORDER_EXAMPLE_PATH, "--config",
       YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, "--market-data-config",
       YQUANT_TEST_MARKET_DATA_CONFIG_PATH, "--trading-config", ""},
      {"--strategy", YQUANT_TEST_ORDER_EXAMPLE_PATH, "--config",
       YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, "--market-data-config",
       YQUANT_TEST_MARKET_DATA_CONFIG_PATH, "--trading-config",
       YQUANT_TEST_TRADING_CONFIG_PATH, "--trading-config",
       YQUANT_TEST_TRADING_CONFIG_PATH},
  };
  for (const auto& arguments : invalid) {
    SCOPED_TRACE(::testing::PrintToString(arguments));
    Process child;
    ASSERT_EQ(child.Start(arguments), 0);
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 2) << child.output();
    EXPECT_TRUE(child.events().empty());
  }
}

TEST(TradingProcessTest, CallbackFailureStillDrainsAndCallsStopOnce) {
  Process child;
  ASSERT_EQ(
      child.Start({"--strategy", YQUANT_TEST_TRADING_UPDATE_FAIL_PATH,
                   "--config", YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                   "--market-data-config", YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                   "--trading-config", YQUANT_TEST_TRADING_CONFIG_PATH}),
      0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("strategy_id=test-trading"), std::string::npos);
  EXPECT_EQ(std::count(child.events().begin(), child.events().end(), 'A'), 1);
  EXPECT_EQ(std::count(child.events().begin(), child.events().end(), 'V'), 0);
  EXPECT_EQ(std::count(child.events().begin(), child.events().end(), 'F'), 0);
  EXPECT_EQ(std::count(child.events().begin(), child.events().end(), 'T'), 1);
  EXPECT_EQ(std::count(child.events().begin(), child.events().end(), 'D'), 1);
}

}  // namespace
}  // namespace yquant::strategyd
