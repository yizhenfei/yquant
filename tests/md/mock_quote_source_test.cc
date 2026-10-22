#include "yquant/md/mock_quote_source.h"

#include <array>
#include <limits>

#include "gtest/gtest.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/quote_tick.h"

namespace yquant::md {
namespace {

MockQuoteConfig Config() {
  MockQuoteConfig config;
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

TEST(MockQuoteSourceTest, EmitsInConfigOrderAndAlternatesWholeBatches) {
  const auto config = Config();
  MockQuoteSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start(5).ok());
  EXPECT_EQ(source.next_deadline(), config.interval_ns + 5);
  model::QuoteTick tick;
  for (std::uint64_t batch = 0; batch < 3; ++batch) {
    const auto now = (batch + 1) * config.interval_ns + 5;
    for (std::size_t i = 0; i < 2; ++i) {
      ASSERT_EQ(source.Poll(now, tick).state, MockQuotePollState::kEvent);
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

TEST(MockQuoteSourceTest, IdleAndErrorsPreserveCallerOutput) {
  auto config = Config();
  config.instrument_count = 1;
  MockQuoteSource source;
  model::QuoteTick tick;
  tick.sequence = 99;
  tick.bid_price = 31337;
  const auto unchanged = tick;
  EXPECT_EQ(source.Poll(0, tick).state, MockQuotePollState::kError);
  EXPECT_EQ(tick, unchanged);
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start(100).ok());
  EXPECT_EQ(source.Poll(100 + config.interval_ns - 1, tick).state,
            MockQuotePollState::kIdle);
  EXPECT_EQ(tick, unchanged);
  ASSERT_TRUE(source.Stop().ok());
  EXPECT_EQ(source.Poll(100000000, tick).state, MockQuotePollState::kError);
  EXPECT_EQ(tick, unchanged);
}

TEST(MockQuoteSourceTest, DelayedPollSchedulesFromActualBatchCompletion) {
  auto config = Config();
  MockQuoteSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start(0).ok());
  model::QuoteTick tick;
  constexpr std::uint64_t late = 10000000000;
  ASSERT_EQ(source.Poll(late, tick).state, MockQuotePollState::kEvent);
  EXPECT_EQ(source.next_deadline(), late);
  ASSERT_EQ(source.Poll(late + 7, tick).state, MockQuotePollState::kEvent);
  EXPECT_EQ(tick.sequence, 1U);
  EXPECT_EQ(source.next_deadline(), late + 7 + config.interval_ns);
  EXPECT_EQ(source.Poll(late + 7, tick).state, MockQuotePollState::kIdle);
}

TEST(MockQuoteSourceTest, CopiesConfiguration) {
  auto config = Config();
  const auto expected = config.instruments[0];
  MockQuoteSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const auto interval = config.interval_ns;
  ASSERT_TRUE(source.Start(0).ok());
  config = {};
  model::QuoteTick tick;
  ASSERT_EQ(source.Poll(interval, tick).state, MockQuotePollState::kEvent);
  EXPECT_EQ(tick.instrument_id, expected.instrument.id);
  EXPECT_EQ(tick.bid_price, expected.bid_price);
}

TEST(MockQuoteSourceTest, StopIsIdempotentAndClearsTheSchedule) {
  MockQuoteSource source;
  EXPECT_EQ(source.Start(0).code, base::ErrorCode::kInvalidState);
  ASSERT_TRUE(source.Prepare(Config()).ok());
  ASSERT_TRUE(source.Start(0).ok());
  EXPECT_TRUE(source.next_deadline());
  EXPECT_EQ(source.Start(0).code, base::ErrorCode::kInvalidState);
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_FALSE(source.next_deadline());
  model::QuoteTick tick;
  EXPECT_EQ(source.Poll(UINT64_MAX, tick).state, MockQuotePollState::kError);
}

TEST(MockQuoteSourceTest, RejectsInvalidTypedConfigBeforeCopyingIt) {
  MockQuoteSource source;
  auto config = Config();
  config.instruments[0].price_step = std::numeric_limits<std::int64_t>::max();
  EXPECT_EQ(source.Prepare(config).code, base::ErrorCode::kInvalidArgument);
  EXPECT_NE(source.error_detail()[0], '\0');
  EXPECT_TRUE(source.Prepare(Config()).ok());
}

TEST(MockQuoteSourceTest, DetectsDeadlineOverflowWithoutWritingOutput) {
  auto config = Config();
  config.instrument_count = 1;
  MockQuoteSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  EXPECT_EQ(source.Start(UINT64_MAX - config.interval_ns + 1).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_FALSE(source.next_deadline().has_value());
  ASSERT_TRUE(source.Start(UINT64_MAX - config.interval_ns).ok());
  model::QuoteTick tick;
  tick.sequence = 77;
  const auto original = tick;
  const auto result = source.Poll(UINT64_MAX, tick);
  EXPECT_EQ(result.state, MockQuotePollState::kError);
  EXPECT_EQ(result.status.code, base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(tick, original);
  EXPECT_FALSE(source.next_deadline().has_value());
  EXPECT_TRUE(source.Stop().ok());
  EXPECT_NE(source.error_detail()[0], '\0');
}

}  // namespace
}  // namespace yquant::md
