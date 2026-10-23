#include <stdint.h>

#include <charconv>
#include <csignal>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "test_config_file.h"
#include "trading_process_helpers.h"
#include "yquant/md/market_data_config.h"

namespace yquant::strategyd {
namespace {

// Copies the shipped replay configuration, retaining its data/catalog paths
// when the temporary configuration moves to another directory.
std::string AcceleratedReplay() {
  md::MarketDataConfigLoader loader;
  if (!loader.Load(YQUANT_TEST_FUTURES_MARKET_CONFIG_PATH).ok()) {
    return {};
  }
  std::ifstream input(YQUANT_TEST_FUTURES_MARKET_CONFIG_PATH);
  std::string json{std::istreambuf_iterator<char>(input), {}};
  const auto replace = [&](std::string_view before, std::string_view after) {
    const auto offset = json.find(before);
    if (offset == std::string::npos) {
      return false;
    }
    json.replace(offset, before.size(), after);
    return true;
  };
  const auto quote_path = [](std::string_view path) {
    std::string result = "\"";
    for (const char byte : path) {
      if (byte == '"' || byte == '\\') {
        result += '\\';
      }
      result += byte;
    }
    return result + '"';
  };
  const auto catalog_path = ::yquant::testing::files::AbsolutePath(
      ::yquant::testing::files::FilePath("examples/instruments.json"));
  if (!replace("\"examples/data/a1805_20171227.csv\"",
               quote_path(loader.config().replay_path)) ||
      !replace("\"examples/instruments.json\"",
               quote_path(catalog_path.native())) ||
      !replace("\"replay_speed\": 100", "\"replay_speed\": 1000")) {
    return {};
  }
  return json;
}

std::vector<std::string> FuturesArguments(const char* market_path) {
  return {"--strategy",
          YQUANT_TEST_FUTURES_BBO_PATH,
          "--config",
          YQUANT_TEST_FUTURES_STRATEGY_CONFIG_PATH,
          "--market-data-config",
          market_path,
          "--trading-config",
          YQUANT_TEST_FUTURES_TRADING_CONFIG_PATH};
}

// Reads one complete numeric field from the final summary, independently of
// the number and order of fields in readiness messages.
int64_t SummaryValue(std::string_view output, std::string_view name) {
  const auto stopped = output.rfind("futures_bbo: stopped ");
  if (stopped == output.npos) {
    ADD_FAILURE() << output;
    return INT64_MIN;
  }
  output.remove_prefix(stopped);
  output = output.substr(0, output.find('\n'));
  const auto key = " " + std::string(name) + '=';
  auto offset = output.find(key);
  if (offset == output.npos) {
    ADD_FAILURE() << "missing " << name << ": " << output;
    return INT64_MIN;
  }
  offset += key.size();
  const auto value = output.substr(offset);
  int64_t number = INT64_MIN;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), number);
  EXPECT_EQ(parsed.ec, std::errc{});
  return number;
}

// Draining releases outstanding orders; already executed inventory and costs
// remain visible, including a signal interrupting the middle of a round trip.
void ExpectDrained(const trading_testing::Process& child) {
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find("strategy_id=futures-bbo-example"),
            std::string::npos);
  EXPECT_EQ(SummaryValue(child.output(), "reserved"), 0);
  EXPECT_EQ(SummaryValue(child.output(), "frozen_qty"), 0);
  EXPECT_EQ(SummaryValue(child.output(), "slots"), 0);
  const auto net = SummaryValue(child.output(), "net");
  EXPECT_GE(net, -1);
  EXPECT_LE(net, 1);
  EXPECT_LE(SummaryValue(child.output(), "max_gross_seen"), 2);
  EXPECT_EQ(child.output().find("unconfirmed_order="), std::string::npos);
}

TEST(FuturesProcessTest, RealHistoricalQuotesFinishAndDrainWithoutSignal) {
  const auto json = AcceleratedReplay();
  ASSERT_FALSE(json.empty());
  testing::ConfigFile market(json);
  ASSERT_TRUE(market.valid());
  trading_testing::Process child;
  ASSERT_EQ(child.Start(FuturesArguments(market.path())), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  ExpectDrained(child);
  EXPECT_NE(child.output().find("futures_bbo: first fill received\n"),
            std::string::npos);
  EXPECT_NE(child.output().find("futures_bbo: round trip completed\n"),
            std::string::npos);
  const auto quantity = SummaryValue(child.output(), "filled_qty");
  const auto commission = SummaryValue(child.output(), "commission");
  EXPECT_GT(quantity, 0);
  EXPECT_EQ(commission, quantity * 2000000);
  const auto balance = SummaryValue(child.output(), "balance");
  const auto equity = SummaryValue(child.output(), "equity");
  const auto margin = SummaryValue(child.output(), "margin");
  EXPECT_EQ(
      balance,
      100000000000 + SummaryValue(child.output(), "realized_pnl") - commission);
  EXPECT_EQ(equity, balance + SummaryValue(child.output(), "unrealized_pnl"));
  EXPECT_EQ(SummaryValue(child.output(), "available"), equity - margin);
}

TEST(FuturesProcessTest, SignalAfterBothQuotesCancelsAndKeepsFinalSummary) {
  trading_testing::Process child;
  ASSERT_EQ(
      child.Start(FuturesArguments(YQUANT_TEST_FUTURES_MARKET_CONFIG_PATH)), 0);
  ASSERT_TRUE(child.WaitForOutput("futures_bbo: first quotes submitted\n"))
      << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  ExpectDrained(child);
}

}  // namespace
}  // namespace yquant::strategyd
