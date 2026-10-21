#include <cinttypes>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "trading_process_helpers.h"

namespace yquant::strategyd {
namespace {

using trading_testing::Process;

std::vector<std::string> BboArguments() {
  return {"--strategy",
          YQUANT_TEST_BBO_EXAMPLE_PATH,
          "--config",
          YQUANT_TEST_BBO_EXAMPLE_CONFIG_PATH,
          "--market-data-config",
          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
          "--trading-config",
          YQUANT_TEST_BBO_TRADING_CONFIG_PATH};
}

// Counts complete readiness/lifecycle markers, independently of other output.
std::size_t CountMarkers(std::string_view output,
                         std::string_view marker) noexcept {
  std::size_t count = 0;
  std::size_t offset = 0;
  while ((offset = output.find(marker, offset)) != std::string_view::npos) {
    ++count;
    offset += marker.size();
  }
  return count;
}

TEST(BboProcessTest, LoadsPluginAndDrainsQuotesAfterTerminationSignal) {
  Process child;
  ASSERT_EQ(child.Start(BboArguments()), 0);
  // This marker follows the first successful reconciliation. Waiting for it
  // exercises real quote callbacks without guessing readiness from a sleep.
  ASSERT_TRUE(
      child.WaitForOutput("bbo_market_making: first quotes submitted\n"))
      << child.output();
  ASSERT_TRUE(child.Signal(SIGTERM));
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find("Running"), std::string::npos);
  EXPECT_NE(child.output().find("strategy_id=bbo-market-making-example"),
            std::string::npos);
  EXPECT_EQ(CountMarkers(child.output(), "bbo_market_making: ready "), 1);
  EXPECT_EQ(CountMarkers(child.output(),
                         "bbo_market_making: first quotes submitted\n"),
            1);
  constexpr std::string_view kStopped = "bbo_market_making: stopped ";
  ASSERT_EQ(CountMarkers(child.output(), kStopped), 1) << child.output();
  const auto offset = child.output().find(kStopped);
  const auto line =
      child.output().substr(offset, child.output().find('\n', offset) - offset);
  std::uint64_t submitted = 0;
  std::uint64_t cancels = 0;
  std::uint64_t fills = 0;
  std::uint64_t filled_qty = 0;
  std::int64_t cash = 0;
  std::int64_t cash_reserved = 0;
  std::int64_t total_qty = 0;
  std::int64_t available_qty = 0;
  std::int64_t reserved_qty = 0;
  unsigned slots = 0;
  ASSERT_EQ(std::sscanf(line.c_str(),
                        "bbo_market_making: stopped submitted=%" SCNu64
                        " cancel_requests=%" SCNu64 " fills=%" SCNu64
                        " filled_qty=%" SCNu64 " cash_balance=%" SCNd64
                        " cash_reserved=%" SCNd64 " total_qty=%" SCNd64
                        " available_qty=%" SCNd64 " reserved_qty=%" SCNd64
                        " slots=%u",
                        &submitted, &cancels, &fills, &filled_qty, &cash,
                        &cash_reserved, &total_qty, &available_qty,
                        &reserved_qty, &slots),
            10)
      << line;
  // The working mock never fills. Both initial sides must have been submitted,
  // and final host draining must release all cash/share reservations.
  EXPECT_GE(submitted, 2);
  EXPECT_EQ(fills, 0);
  EXPECT_EQ(filled_qty, 0);
  EXPECT_EQ(cash, 30000000000);
  EXPECT_EQ(cash_reserved, 0);
  EXPECT_EQ(total_qty, 1000);
  EXPECT_EQ(available_qty, 800);
  EXPECT_EQ(reserved_qty, 0);
  EXPECT_EQ(slots, 0);
  EXPECT_EQ(child.output().find("unconfirmed_order="), std::string::npos);
}

TEST(BboProcessTest, MissingTradingAccountFailsBeforeRunningOrStopCallback) {
  auto arguments = BboArguments();
  arguments.resize(arguments.size() - 2);
  Process child;
  ASSERT_EQ(child.Start(arguments), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 1) << child.output();
  EXPECT_NE(child.output().find("stage=start"), std::string::npos);
  EXPECT_NE(child.output().find("strategy OnStart returned an error"),
            std::string::npos);
  EXPECT_EQ(child.output().find("Running"), std::string::npos);
  EXPECT_EQ(child.output().find("bbo_market_making: ready"), std::string::npos);
  EXPECT_EQ(child.output().find("bbo_market_making: first quotes submitted"),
            std::string::npos);
  EXPECT_EQ(child.output().find("bbo_market_making: stopped"),
            std::string::npos);
}

}  // namespace
}  // namespace yquant::strategyd
