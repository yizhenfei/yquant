#include "strategyd/mock_market_data_source.h"

#include <array>
#include <limits>

#include "gtest/gtest.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/quote_tick.h"

namespace yquant::strategyd {
namespace {

MarketDataConfig Config() {
  MarketDataConfig config;
  config.source_id = {42};
  config.interval_ns = 1000000;
  config.instrument_count = 2;
  for (std::size_t i = 0; i < config.instrument_count; ++i) {
    auto& item = config.instruments[i];
    item.instrument.venue = model::Venue::kSse;
    const auto symbol = i == 0 ? "600000" : "600001";
    EXPECT_EQ(model::MakeInstrumentId("SSE", symbol, item.instrument.id),
              base::ErrorCode::kOk);
    for (std::size_t digit = 0; digit < model::kSymbolLength; ++digit)
      item.instrument.symbol[digit] = symbol[digit];
    item.instrument.price_decimals = 2;
    item.bid_price = 1000 + i;
    item.ask_price = 1002 + i;
    item.bid_qty = 100;
    item.ask_qty = 200;
    item.price_step = 3;
  }
  return config;
}

TEST(MockMarketDataSourceTest, EmitsInConfigOrderAndAlternatesWholeBatches) {
  const auto config = Config();
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const std::array ids = {config.instruments[1].instrument.id,
                          config.instruments[0].instrument.id};
  ASSERT_TRUE(source.Start(ids, 5).ok());
  EXPECT_EQ(source.next_deadline(), config.interval_ns + 5);
  model::QuoteTick tick;
  for (std::uint64_t batch = 0; batch < 3; ++batch) {
    const auto now = (batch + 1) * config.interval_ns + 5;
    for (std::size_t i = 0; i < 2; ++i) {
      ASSERT_EQ(source.Poll(now, tick).state, MarketDataPollState::kEvent);
      EXPECT_EQ(tick.source_id, config.source_id);
      EXPECT_EQ(tick.instrument_id, config.instruments[i].instrument.id);
      EXPECT_EQ(tick.sequence, batch + 1);
      EXPECT_EQ(tick.receive_time_ns, now);
      EXPECT_GT(tick.event_time_ns, 0U);
      EXPECT_EQ(tick.flags, model::kKnownQuoteFlags);
      EXPECT_EQ(tick.bid_price, config.instruments[i].bid_price +
                                    static_cast<std::int64_t>(batch % 2) * 3);
      EXPECT_EQ(tick.ask_price, config.instruments[i].ask_price +
                                    static_cast<std::int64_t>(batch % 2) * 3);
      EXPECT_EQ(tick.bid_qty, 100);
      EXPECT_EQ(tick.ask_qty, 200);
    }
    EXPECT_EQ(source.next_deadline(), now + config.interval_ns);
  }
}

TEST(MockMarketDataSourceTest, IdleAndErrorsPreserveCallerOutput) {
  auto config = Config();
  config.instrument_count = 1;
  MockMarketDataSource source;
  model::QuoteTick tick;
  tick.sequence = 99;
  tick.bid_price = 31337;
  const auto unchanged = tick;
  EXPECT_EQ(source.Poll(0, tick).state, MarketDataPollState::kError);
  EXPECT_EQ(tick, unchanged);
  ASSERT_TRUE(source.Prepare(config).ok());
  const std::array ids = {config.instruments[0].instrument.id};
  ASSERT_TRUE(source.Start(ids, 100).ok());
  EXPECT_EQ(source.Poll(100 + config.interval_ns - 1, tick).state,
            MarketDataPollState::kIdle);
  EXPECT_EQ(tick, unchanged);
  ASSERT_TRUE(source.Stop().ok());
  EXPECT_EQ(source.Poll(100000000, tick).state, MarketDataPollState::kError);
  EXPECT_EQ(tick, unchanged);
}

TEST(MockMarketDataSourceTest, DelayedPollSchedulesFromActualBatchCompletion) {
  auto config = Config();
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const std::array ids = {config.instruments[0].instrument.id,
                          config.instruments[1].instrument.id};
  ASSERT_TRUE(source.Start(ids, 0).ok());
  model::QuoteTick tick;
  constexpr std::uint64_t late = 10000000000;
  ASSERT_EQ(source.Poll(late, tick).state, MarketDataPollState::kEvent);
  EXPECT_EQ(source.next_deadline(), late);
  ASSERT_EQ(source.Poll(late + 7, tick).state, MarketDataPollState::kEvent);
  EXPECT_EQ(tick.sequence, 1U);
  EXPECT_EQ(source.next_deadline(), late + 7 + config.interval_ns);
  EXPECT_EQ(source.Poll(late + 7, tick).state, MarketDataPollState::kIdle);
}

TEST(MockMarketDataSourceTest, CopiesConfigurationAndSubscriptions) {
  auto config = Config();
  const auto expected = config.instruments[0];
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  std::array ids = {config.instruments[0].instrument.id};
  const auto interval = config.interval_ns;
  ASSERT_TRUE(source.Start(ids, 0).ok());
  config = {};
  ids[0] = {};
  model::QuoteTick tick;
  ASSERT_EQ(source.Poll(interval, tick).state, MarketDataPollState::kEvent);
  EXPECT_EQ(tick.instrument_id, expected.instrument.id);
  EXPECT_EQ(tick.bid_price, expected.bid_price);
}

TEST(MockMarketDataSourceTest,
     EmptySubscriptionsRemainIdleAndStopIsIdempotent) {
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(Config()).ok());
  ASSERT_TRUE(source.Start({}, 0).ok());
  EXPECT_FALSE(source.next_deadline().has_value());
  model::QuoteTick tick;
  EXPECT_EQ(source.Poll(UINT64_MAX, tick).state, MarketDataPollState::kIdle);
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_FALSE(source.next_deadline().has_value());
}

TEST(MockMarketDataSourceTest, RejectsInvalidActivationWithoutCommittingIt) {
  const auto config = Config();
  MockMarketDataSource source;
  EXPECT_EQ(source.Start({}, 0).code, base::ErrorCode::kInvalidState);
  ASSERT_TRUE(source.Prepare(config).ok());
  const std::array duplicate = {config.instruments[0].instrument.id,
                                config.instruments[0].instrument.id};
  EXPECT_EQ(source.Start(duplicate, 0).code, base::ErrorCode::kInvalidArgument);
  EXPECT_FALSE(source.next_deadline().has_value());
  const std::array unknown = {model::InstrumentId{123}};
  EXPECT_EQ(source.Start(unknown, 0).code, base::ErrorCode::kInvalidArgument);
  const std::array valid = {config.instruments[1].instrument.id};
  EXPECT_TRUE(source.Start(valid, 0).ok());
  EXPECT_EQ(source.Start(valid, 0).code, base::ErrorCode::kInvalidState);
}

TEST(MockMarketDataSourceTest, RejectsInvalidTypedConfigBeforeCopyingIt) {
  MockMarketDataSource source;
  auto config = Config();
  config.instruments[0].price_step = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(source.Prepare(config).code, base::ErrorCode::kInvalidArgument);
  EXPECT_NE(source.error_detail()[0], '\0');
  EXPECT_TRUE(source.Prepare(Config()).ok());
}

TEST(MockMarketDataSourceTest, DetectsDeadlineOverflowWithoutWritingOutput) {
  auto config = Config();
  config.instrument_count = 1;
  MockMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const std::array ids = {config.instruments[0].instrument.id};
  EXPECT_EQ(source.Start(ids, UINT64_MAX - config.interval_ns + 1).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_FALSE(source.next_deadline().has_value());
  ASSERT_TRUE(source.Start(ids, UINT64_MAX - config.interval_ns).ok());
  model::QuoteTick tick;
  tick.sequence = 77;
  const auto original = tick;
  const auto result = source.Poll(UINT64_MAX, tick);
  EXPECT_EQ(result.state, MarketDataPollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(tick, original);
  EXPECT_FALSE(source.next_deadline().has_value());
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_NE(source.error_detail()[0], '\0');
}

}  // namespace
}  // namespace yquant::strategyd
