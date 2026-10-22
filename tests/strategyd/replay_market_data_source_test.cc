#include "strategyd/replay_market_data_source.h"

#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "test_config_file.h"

namespace yquant::strategyd {
namespace {

constexpr std::string_view kHeader =
    "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags\n";
constexpr std::string_view kRow = "0,0,3500,10,3502,20,7\n";

md::MarketDataConfig ReplayConfig(const char* path) {
  md::MarketDataConfig config;
  config.adapter = md::MarketDataAdapter::kReplay;
  config.source_id = {42};
  config.instrument_count = 1;
  std::strcpy(config.replay_path, path);
  auto& definition = config.instruments[0].instrument;
  EXPECT_EQ(model::MakeInstrumentId("SHFE", "rb2701", definition.id),
            base::ErrorCode::kOk);
  definition.venue = model::Venue::kShfe;
  std::memcpy(definition.symbol, "rb2701", 6);
  definition.type = model::InstrumentType::kFutures;
  definition.price_tick = 2;
  definition.quantity_step = 5;
  definition.contract_multiplier = 10;
  definition.margin_rate_ppm = 100000;
  return config;
}

TEST(ReplayMarketDataSourceTest, UsesAbsoluteScaledTimesAndPreservesInputUtc) {
  testing::ConfigFile csv(std::string(kHeader) +
                          "4,123,3500,10,3502,20,15\n4,124,3502,15,3504,25,15\n"
                          "10,125,3504,20,3506,30,15\n");
  ASSERT_TRUE(csv.valid());
  auto config = ReplayConfig(csv.path());
  config.replay_speed = 2;
  config.replay_tail_ns = 7;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok()) << source.error_detail();
  const auto id = config.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 100).ok());
  EXPECT_EQ(source.next_deadline(), 102U);
  model::QuoteTick quote;
  quote.sequence = 99;
  const auto sentinel = quote;
  EXPECT_EQ(source.Poll(101, quote).state, MarketDataPollState::kIdle);
  EXPECT_EQ(quote, sentinel);
  ASSERT_EQ(source.Poll(102, quote).state, MarketDataPollState::kEvent);
  EXPECT_EQ(quote.sequence, 1U);
  EXPECT_EQ(quote.event_time_ns, 123U);
  EXPECT_EQ(quote.receive_time_ns, 102U);
  EXPECT_EQ(quote.source_id, config.source_id);
  EXPECT_EQ(quote.instrument_id, id);
  EXPECT_EQ(source.next_deadline(), 102U);
  ASSERT_EQ(source.Poll(103, quote).state, MarketDataPollState::kEvent);
  EXPECT_EQ(quote.sequence, 2U);
  EXPECT_EQ(quote.receive_time_ns, 103U);
  EXPECT_EQ(source.next_deadline(), 105U);
  // Late polls retain the original schedule rather than moving EOF forward.
  ASSERT_EQ(source.Poll(120, quote).state, MarketDataPollState::kEvent);
  EXPECT_EQ(quote.sequence, 3U);
  EXPECT_EQ(source.next_deadline(), 112U);
  EXPECT_FALSE(source.finished());
  const auto last = quote;
  EXPECT_EQ(source.Poll(120, quote).state, MarketDataPollState::kIdle);
  EXPECT_EQ(quote, last);
  EXPECT_TRUE(source.finished());
  EXPECT_FALSE(source.next_deadline());
}

TEST(ReplayMarketDataSourceTest,
     FinishesAfterTailAndAlwaysDeliversLastFrameFirst) {
  testing::ConfigFile csv(std::string(kHeader) + std::string(kRow) +
                          "10,0,3500,10,3502,20,7\n");
  ASSERT_TRUE(csv.valid());
  auto config = ReplayConfig(csv.path());
  config.replay_tail_ns = 5;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const auto id = config.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  model::QuoteTick quote;
  ASSERT_EQ(source.Poll(0, quote).state, MarketDataPollState::kEvent);
  ASSERT_EQ(source.Poll(10, quote).state, MarketDataPollState::kEvent);
  EXPECT_FALSE(source.finished());
  EXPECT_EQ(source.next_deadline(), 15U);
  const auto last = quote;
  EXPECT_EQ(source.Poll(14, quote).state, MarketDataPollState::kIdle);
  EXPECT_FALSE(source.finished());
  EXPECT_EQ(source.Poll(15, quote).state, MarketDataPollState::kIdle);
  EXPECT_TRUE(source.finished());
  EXPECT_EQ(quote, last);
  EXPECT_EQ(source.Poll(16, quote).state, MarketDataPollState::kIdle);
  EXPECT_EQ(quote, last);
  EXPECT_FALSE(source.next_deadline());
}

TEST(ReplayMarketDataSourceTest,
     NoSubscriptionPublishesNothingButStillFinishes) {
  testing::ConfigFile csv(std::string(kHeader) + "10,0,3500,10,3502,20,7\n");
  ASSERT_TRUE(csv.valid());
  auto config = ReplayConfig(csv.path());
  config.replay_tail_ns = 5;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  ASSERT_TRUE(source.Start({}, 100).ok());
  EXPECT_EQ(source.next_deadline(), 115U);
  model::QuoteTick quote;
  quote.sequence = 77;
  const auto sentinel = quote;
  EXPECT_EQ(source.Poll(114, quote).state, MarketDataPollState::kIdle);
  EXPECT_FALSE(source.finished());
  EXPECT_EQ(source.Poll(115, quote).state, MarketDataPollState::kIdle);
  EXPECT_TRUE(source.finished());
  EXPECT_EQ(quote, sentinel);
}

TEST(ReplayMarketDataSourceTest, OwnsFramesAfterPrepareAndValidatesLifecycle) {
  ReplayMarketDataSource source;
  model::QuoteTick quote;
  quote.sequence = 91;
  const auto sentinel = quote;
  EXPECT_EQ(source.Poll(0, quote).state, MarketDataPollState::kError);
  EXPECT_EQ(quote, sentinel);
  EXPECT_EQ(source.Start({}, 0).code, base::ErrorCode::kInvalidState);
  model::InstrumentId id;
  {
    testing::ConfigFile csv(std::string(kHeader) + std::string(kRow));
    ASSERT_TRUE(csv.valid());
    auto config = ReplayConfig(csv.path());
    config.replay_tail_ns = 0;
    id = config.instruments[0].instrument.id;
    ASSERT_TRUE(source.Prepare(config).ok());
    EXPECT_EQ(source.Prepare(config).code, base::ErrorCode::kInvalidState);
  }
  const model::InstrumentId unknown{123};
  EXPECT_EQ(source.Start(std::span(&unknown, 1), 0).code,
            base::ErrorCode::kInvalidArgument);
  const std::array duplicate{id, id};
  EXPECT_EQ(source.Start(duplicate, 0).code, base::ErrorCode::kInvalidArgument);
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  EXPECT_EQ(source.Start({}, 10).code, base::ErrorCode::kInvalidState);
  EXPECT_EQ(source.next_deadline(), 0U);
  ASSERT_EQ(source.Poll(0, quote).state, MarketDataPollState::kEvent);
  EXPECT_EQ(quote.bid_price, 3500);
  EXPECT_FALSE(source.finished());
  EXPECT_EQ(source.Poll(0, quote).state, MarketDataPollState::kIdle);
  EXPECT_TRUE(source.finished());
  ASSERT_TRUE(source.Stop().ok());
  ASSERT_TRUE(source.Stop().ok());
  EXPECT_FALSE(source.next_deadline());
  const auto last = quote;
  EXPECT_EQ(source.Poll(1, quote).state, MarketDataPollState::kError);
  EXPECT_EQ(quote, last);
}

TEST(ReplayMarketDataSourceTest, StartOverflowLeavesPreparedStateRetryable) {
  testing::ConfigFile csv(std::string(kHeader) +
                          "18446744073709551615,0,3500,10,3502,20,7\n");
  ASSERT_TRUE(csv.valid());
  auto config = ReplayConfig(csv.path());
  config.replay_tail_ns = 0;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  const auto id = config.instruments[0].instrument.id;
  EXPECT_EQ(source.Start(std::span(&id, 1), 1).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_FALSE(source.next_deadline());
  EXPECT_FALSE(source.finished());
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  model::QuoteTick quote;
  ASSERT_EQ(source.Poll(std::numeric_limits<std::uint64_t>::max(), quote).state,
            MarketDataPollState::kEvent);
  EXPECT_FALSE(source.finished());
  EXPECT_EQ(source.Poll(std::numeric_limits<std::uint64_t>::max(), quote).state,
            MarketDataPollState::kIdle);
  EXPECT_TRUE(source.finished());
}

TEST(ReplayMarketDataSourceTest, TailOverflowAlsoPreservesPreparedState) {
  testing::ConfigFile csv(std::string(kHeader) + "5,0,3500,10,3502,20,7\n");
  ASSERT_TRUE(csv.valid());
  auto config = ReplayConfig(csv.path());
  config.replay_tail_ns = 5;
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(config).ok());
  EXPECT_EQ(
      source.Start({}, std::numeric_limits<std::uint64_t>::max() - 9).code,
      base::ErrorCode::kResourceExhausted);
  EXPECT_FALSE(source.next_deadline());
  ASSERT_TRUE(
      source.Start({}, std::numeric_limits<std::uint64_t>::max() - 10).ok());
  EXPECT_EQ(source.next_deadline(), std::numeric_limits<std::uint64_t>::max());
}

struct InvalidCsv {
  const char* name;
  std::string text;
};
class InvalidReplayCsvTest : public ::testing::TestWithParam<InvalidCsv> {};

TEST_P(InvalidReplayCsvTest, RejectsInputWithoutCommittingPreparedState) {
  testing::ConfigFile invalid(GetParam().text);
  testing::ConfigFile valid(std::string(kHeader) + std::string(kRow));
  ASSERT_TRUE(invalid.valid());
  ASSERT_TRUE(valid.valid());
  ReplayMarketDataSource source;
  EXPECT_EQ(source.Prepare(ReplayConfig(invalid.path())).code,
            base::ErrorCode::kInvalidArgument)
      << source.error_detail();
  EXPECT_NE(source.error_detail()[0], '\0');
  EXPECT_FALSE(source.next_deadline());
  EXPECT_FALSE(source.finished());
  EXPECT_TRUE(source.Prepare(ReplayConfig(valid.path())).ok());
}

INSTANTIATE_TEST_SUITE_P(
    Csv, InvalidReplayCsvTest,
    ::testing::Values(
        InvalidCsv{"Empty", ""}, InvalidCsv{"HeaderOnly", std::string(kHeader)},
        InvalidCsv{"WrongHeader",
                   "elapsed,utc,bid,qty,ask,qty,flags\n" + std::string(kRow)},
        InvalidCsv{"BlankRow", std::string(kHeader) + "\n"},
        InvalidCsv{"ExtraColumn",
                   std::string(kHeader) + "0,0,3500,10,3502,20,7,1\n"},
        InvalidCsv{"MissingColumn",
                   std::string(kHeader) + "0,0,3500,10,3502,20\n"},
        InvalidCsv{"EmptyColumn",
                   std::string(kHeader) + "0,0,3500,,3502,20,7\n"},
        InvalidCsv{"Whitespace",
                   std::string(kHeader) + " 0,0,3500,10,3502,20,7\n"},
        InvalidCsv{"PlusSign",
                   std::string(kHeader) + "+0,0,3500,10,3502,20,7\n"},
        InvalidCsv{"Fraction",
                   std::string(kHeader) + "0,0,3500.0,10,3502,20,7\n"},
        InvalidCsv{"NegativeElapsed",
                   std::string(kHeader) + "-1,0,3500,10,3502,20,7\n"},
        InvalidCsv{"ElapsedOverflow",
                   std::string(kHeader) +
                       "18446744073709551616,0,3500,10,3502,20,7\n"},
        InvalidCsv{
            "PriceOverflow",
            std::string(kHeader) + "0,0,9223372036854775808,10,3502,20,7\n"},
        InvalidCsv{"PriceTick",
                   std::string(kHeader) + "0,0,3501,10,3502,20,7\n"},
        InvalidCsv{"QuantityStep",
                   std::string(kHeader) + "0,0,3500,11,3502,20,7\n"},
        InvalidCsv{"MissingSideNonzero",
                   std::string(kHeader) + "0,0,3500,10,3502,20,6\n"},
        InvalidCsv{"PresentSideEmpty",
                   std::string(kHeader) + "0,0,3500,0,3502,20,7\n"},
        InvalidCsv{"UtcWithoutFlag",
                   std::string(kHeader) + "0,1,3500,10,3502,20,7\n"},
        InvalidCsv{"UnknownFlag",
                   std::string(kHeader) + "0,0,3500,10,3502,20,23\n"},
        InvalidCsv{"FlagOverflow",
                   std::string(kHeader) + "0,0,3500,10,3502,20,256\n"},
        InvalidCsv{"RegressingElapsed",
                   std::string(kHeader) +
                       "2,0,3500,10,3502,20,7\n1,0,3500,10,3502,20,7\n"}),
    [](const ::testing::TestParamInfo<InvalidCsv>& info) {
      return info.param.name;
    });

TEST(ReplayMarketDataSourceTest, AcceptsCrLfFinalLineAndSignedPriceBoundary) {
  std::string csv(kHeader);
  csv.replace(csv.size() - 1, 1, "\r\n");
  csv += "0,0,-9223372036854775808,10,3502,20,7\r\n";
  testing::ConfigFile file(csv);
  ASSERT_TRUE(file.valid());
  ReplayMarketDataSource source;
  auto config = ReplayConfig(file.path());
  ASSERT_TRUE(source.Prepare(config).ok()) << source.error_detail();
  const auto id = config.instruments[0].instrument.id;
  ASSERT_TRUE(source.Start(std::span(&id, 1), 0).ok());
  model::QuoteTick quote;
  ASSERT_EQ(source.Poll(0, quote).state, MarketDataPollState::kEvent);
  EXPECT_EQ(quote.bid_price, std::numeric_limits<std::int64_t>::min());
}

TEST(ReplayMarketDataSourceTest, EnforcesFrameAndFileCapacityBeforeActivation) {
  std::string csv(kHeader);
  for (std::size_t index = 0; index != kMaxReplayFrames; ++index) csv += kRow;
  testing::ConfigFile full(csv);
  ASSERT_TRUE(full.valid());
  ReplayMarketDataSource source;
  ASSERT_TRUE(source.Prepare(ReplayConfig(full.path())).ok());
  ASSERT_TRUE(source.Stop().ok());
  csv += kRow;
  testing::ConfigFile oversized_frames(csv);
  ASSERT_TRUE(oversized_frames.valid());
  ReplayMarketDataSource overflow;
  EXPECT_EQ(overflow.Prepare(ReplayConfig(oversized_frames.path())).code,
            base::ErrorCode::kResourceExhausted);
  testing::ConfigFile oversized_file(std::string(kMaxReplayFileBytes + 1, ' '));
  ASSERT_TRUE(oversized_file.valid());
  EXPECT_EQ(overflow.Prepare(ReplayConfig(oversized_file.path())).code,
            base::ErrorCode::kResourceExhausted);
}

TEST(ReplayMarketDataSourceTest, RejectsMissingAndNonregularFiles) {
  ReplayMarketDataSource source;
  auto config = ReplayConfig("/tmp");
  EXPECT_EQ(source.Prepare(config).code, base::ErrorCode::kInvalidArgument);
  testing::ConfigFile missing("");
  ASSERT_TRUE(missing.valid());
  config = ReplayConfig(missing.path());
  ASSERT_EQ(unlink(missing.path()), 0);
  const auto status = source.Prepare(config);
  EXPECT_EQ(status.code, base::ErrorCode::kIoError);
  EXPECT_TRUE(status.system_error);
}

}  // namespace
}  // namespace yquant::strategyd
