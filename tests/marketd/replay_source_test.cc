#include "marketd/replay_source.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "gtest/gtest.h"
#include "marketd/recording.h"
#include "test_support.h"

namespace yquant::marketd {
namespace {

constexpr char kCsv[] =
    "Date,Time,SecurityID,SecurityIDSource,OfferPX1,BidPX1,OfferSize1,"
    "BidSize1\n"
    "2020.12.31,09:15:00.000,000557,102,10.01,10.00,200,100\n"
    "2020.12.31,09:15:00.001,000557,102,10.02,10.01,300,200\n";

ReplayConfig CsvConfig(const std::string& path, double speed = 1) {
  ReplayConfig config;
  std::snprintf(config.path, sizeof(config.path), "%s", path.c_str());
  config.format = ReplayFormat::kDolphinDbCsv;
  config.speed = speed;
  config.max_records = 8;
  config.source_id = {77};
  config.price_decimals = 2;
  return config;
}

TEST(ReplaySourceTest, AnchorsAtFirstPollAndPreservesOutputUntilDue) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  ReplaySource source;
  ASSERT_TRUE(source.Init(CsvConfig(directory.File("quotes.csv"))).ok())
      << source.error_detail();

  constexpr std::uint64_t kStart = 9000000000;
  auto out = testing::Quote(999);
  ASSERT_EQ(source.Poll(kStart, out).state, PollState::kEvent);
  EXPECT_EQ(out.source_id.value, 77U);
  EXPECT_EQ(out.sequence, 1U);
  EXPECT_EQ(out.receive_time_ns, kStart);
  EXPECT_EQ(out.bid_price, 1000);
  EXPECT_EQ(out.ask_price, 1001);
  const auto first = out;
  for (const auto now : {kStart, kStart + 1, kStart + 999999}) {
    EXPECT_EQ(source.Poll(now, out).state, PollState::kIdle);
    EXPECT_EQ(out, first);
  }

  ASSERT_EQ(source.Poll(kStart + 1000000, out).state, PollState::kEvent);
  EXPECT_EQ(out.source_id, first.source_id);
  EXPECT_EQ(out.instrument_id, first.instrument_id);
  EXPECT_EQ(out.sequence, 2U);
  EXPECT_EQ(out.event_time_ns - first.event_time_ns, 1000000U);
  EXPECT_EQ(out.receive_time_ns, kStart + 1000000);
  EXPECT_EQ(out.bid_price, 1001);
  EXPECT_EQ(out.ask_price, 1002);
  const auto last = out;
  EXPECT_EQ(source.Poll(kStart + 1000000, out).state, PollState::kEnd);
  EXPECT_EQ(source.Poll(kStart + 2000000, out).state, PollState::kEnd);
  EXPECT_EQ(out, last);
}

TEST(ReplaySourceTest, ScalesArrivalGapsAndZeroSpeedDrainsImmediately) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  struct Case {
    double speed;
    std::uint64_t gap;
  };
  constexpr std::array<Case, 3> kCases = {Case{0.5, 2000000}, Case{10, 100000},
                                          Case{3, 333333}};
  for (const auto& item : kCases) {
    ReplaySource source;
    ASSERT_TRUE(
        source.Init(CsvConfig(directory.File("quotes.csv"), item.speed)).ok())
        << source.error_detail();
    model::QuoteTick out;
    ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
    const auto first = out;
    EXPECT_EQ(source.Poll(100 + item.gap - 1, out).state, PollState::kIdle);
    EXPECT_EQ(out, first);
    ASSERT_EQ(source.Poll(100 + item.gap, out).state, PollState::kEvent);
    EXPECT_EQ(out.sequence, 2U);
  }

  ReplaySource source;
  ASSERT_TRUE(source.Init(CsvConfig(directory.File("quotes.csv"), 0)).ok());
  model::QuoteTick out;
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 2U);
  EXPECT_EQ(out.receive_time_ns, 100U);
  EXPECT_EQ(source.Poll(100, out).state, PollState::kEnd);
}

TEST(ReplaySourceTest, InitialDelayAppliesBeforeBothPacedAndUnpacedReplay) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  for (const double speed : {0.0, 1.0}) {
    auto config = CsvConfig(directory.File("quotes.csv"), speed);
    config.start_delay_ns = 200;
    ReplaySource source;
    ASSERT_TRUE(source.Init(config).ok()) << source.error_detail();
    auto out = testing::Quote(999);
    const auto unchanged = out;
    EXPECT_EQ(source.Poll(100, out).state, PollState::kIdle);
    EXPECT_EQ(source.Poll(299, out).state, PollState::kIdle);
    EXPECT_EQ(out, unchanged);
    ASSERT_EQ(source.Poll(300, out).state, PollState::kEvent);
    EXPECT_EQ(out.sequence, 1U);
    EXPECT_EQ(out.receive_time_ns, 300U);
    const auto first = out;
    if (speed == 0) {
      EXPECT_EQ(source.Poll(300, out).state, PollState::kEvent);
    } else {
      EXPECT_EQ(source.Poll(300, out).state, PollState::kIdle);
      EXPECT_EQ(out, first);
      EXPECT_EQ(source.Poll(1000300, out).state, PollState::kEvent);
    }
    EXPECT_EQ(out.sequence, 2U);
  }
}

TEST(ReplaySourceTest, RecordingReplaysOriginalMetadataAndArrivalOffsets) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  RecordConfig recording;
  std::snprintf(recording.path, sizeof(recording.path), "%s",
                directory.File("quotes.yqr").c_str());
  recording.queue_capacity = 4096;
  RecordPublisher publisher;
  ASSERT_TRUE(publisher.Init(recording).ok()) << publisher.error_detail();
  auto first = testing::Quote(123);
  first.receive_time_ns = 9000000;
  auto second = testing::Quote(124);
  second.receive_time_ns = 10000000;
  second.event_time_ns = first.event_time_ns - 1;
  ASSERT_TRUE(publisher.Publish(first).ok());
  ASSERT_TRUE(publisher.Publish(second).ok());
  ASSERT_TRUE(publisher.Finish().ok()) << publisher.error_detail();

  ReplayConfig config;
  std::memcpy(config.path, recording.path, sizeof(config.path));
  config.max_records = 8;
  ReplaySource source;
  ASSERT_TRUE(source.Init(config).ok()) << source.error_detail();
  model::QuoteTick out;
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  first.receive_time_ns = 100;
  EXPECT_EQ(out, first);
  ASSERT_EQ(source.Poll(1000099, out).state, PollState::kIdle);
  EXPECT_EQ(out, first);
  ASSERT_EQ(source.Poll(1000100, out).state, PollState::kEvent);
  second.receive_time_ns = 1000100;
  EXPECT_EQ(out, second);
  EXPECT_EQ(source.Poll(1000100, out).state, PollState::kEnd);
}

TEST(ReplaySourceTest, EmptyCsvFinishesWithoutTouchingTheOutput) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  constexpr char kHeader[] =
      "Date,Time,SecurityID,SecurityIDSource,OfferPX1,BidPX1,OfferSize1,"
      "BidSize1\n";
  ASSERT_TRUE(directory.Write("quotes.csv", kHeader));
  ReplaySource source;
  ASSERT_TRUE(source.Init(CsvConfig(directory.File("quotes.csv"))).ok())
      << source.error_detail();
  auto out = testing::Quote(999);
  const auto unchanged = out;
  const auto result = source.Poll(1, out);
  EXPECT_EQ(result.state, PollState::kEnd);
  EXPECT_TRUE(result.status.ok());
  EXPECT_EQ(source.Poll(2, out).state, PollState::kEnd);
  EXPECT_EQ(out, unchanged);
}

TEST(ReplaySourceTest, RelativeClockAvoidsAbsoluteDeadlineOverflow) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  ReplaySource source;
  ASSERT_TRUE(source.Init(CsvConfig(directory.File("quotes.csv"))).ok());
  model::QuoteTick out;
  constexpr auto kLast = std::numeric_limits<std::uint64_t>::max();
  ASSERT_EQ(source.Poll(kLast - 100, out).state, PollState::kEvent);
  const auto first = out;
  EXPECT_EQ(source.Poll(kLast, out).state, PollState::kIdle);
  EXPECT_EQ(out, first);
}

TEST(ReplaySourceTest, ClockRegressionIsTerminalAndInitResetsFailure) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  const auto config = CsvConfig(directory.File("quotes.csv"));
  ReplaySource source;
  ASSERT_TRUE(source.Init(config).ok());
  model::QuoteTick out;
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  ASSERT_EQ(source.Poll(200, out).state, PollState::kIdle);
  const auto first = out;
  const auto failed = source.Poll(199, out);
  EXPECT_EQ(failed.state, PollState::kError);
  EXPECT_EQ(failed.status.code, base::ErrorCode::kInvalidArgument);
  EXPECT_NE(std::strstr(source.error_detail(), "backwards"), nullptr);
  EXPECT_EQ(source.Poll(1000100, out).state, PollState::kError);
  EXPECT_EQ(out, first);
  const std::string detail = source.error_detail();
  source.Close();
  EXPECT_EQ(source.error_detail(), detail);
  ASSERT_TRUE(source.Init(config).ok());
  EXPECT_STREQ(source.error_detail(), "");
  ASSERT_EQ(source.Poll(1, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 1U);
}

TEST(ReplaySourceTest, RejectsInvalidOptionsAndUnrepresentableDuration) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  const auto valid = CsvConfig(directory.File("quotes.csv"));
  const std::array<double, 4> speeds = {
      -1, std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      std::numeric_limits<double>::denorm_min()};
  ReplaySource source;
  for (const auto speed : speeds) {
    auto config = valid;
    config.speed = speed;
    EXPECT_EQ(source.Init(config).code, base::ErrorCode::kInvalidArgument);
    auto out = testing::Quote(999);
    const auto unchanged = out;
    EXPECT_EQ(source.Poll(1, out).state, PollState::kError);
    EXPECT_EQ(out, unchanged);
  }
  for (const auto capacity : {std::size_t{0}, kMaxReplayRecords + 1}) {
    auto config = valid;
    config.max_records = capacity;
    EXPECT_EQ(source.Init(config).code, base::ErrorCode::kInvalidArgument);
  }
  auto config = valid;
  config.format = static_cast<ReplayFormat>(999);
  EXPECT_EQ(source.Init(config).code, base::ErrorCode::kInvalidArgument);
  config = valid;
  config.path[0] = '\0';
  EXPECT_EQ(source.Init(config).code, base::ErrorCode::kInvalidArgument);
  std::memset(config.path, 'x', sizeof(config.path));
  EXPECT_EQ(source.Init(config).code, base::ErrorCode::kInvalidArgument);
}

TEST(ReplaySourceTest, ValidatesWholeFileBeforeExposingAnyQuote) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  auto config = CsvConfig(directory.File("quotes.csv"));
  config.max_records = 1;
  ReplaySource source;
  EXPECT_FALSE(source.Init(config).ok());
  auto out = testing::Quote(999);
  const auto unchanged = out;
  EXPECT_EQ(source.Poll(1, out).state, PollState::kError);
  EXPECT_EQ(out, unchanged);

  config.max_records = 8;
  ASSERT_TRUE(source.Init(config).ok());
  ASSERT_EQ(source.Poll(1, out).state, PollState::kEvent);
  config.path[0] = '\0';
  EXPECT_FALSE(source.Init(config).ok());
  const auto first = out;
  EXPECT_EQ(source.Poll(1000001, out).state, PollState::kError);
  EXPECT_EQ(out, first);
}

TEST(ReplaySourceTest, CloseAndReinitializationRestartTheCursor) {
  testing::RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write("quotes.csv", kCsv));
  const auto config = CsvConfig(directory.File("quotes.csv"));
  ReplaySource source;
  auto out = testing::Quote(999);
  const auto unchanged = out;
  EXPECT_EQ(source.Poll(1, out).state, PollState::kError);
  EXPECT_EQ(out, unchanged);
  ASSERT_TRUE(source.Init(config).ok());
  ASSERT_EQ(source.Poll(100, out).state, PollState::kEvent);
  ASSERT_TRUE(source.Init(config).ok());
  ASSERT_EQ(source.Poll(200, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 1U);
  EXPECT_EQ(out.receive_time_ns, 200U);
  source.Close();
  const auto first = out;
  EXPECT_EQ(source.Poll(2000100, out).state, PollState::kError);
  EXPECT_EQ(out, first);
  ASSERT_TRUE(source.Init(config).ok());
  ASSERT_EQ(source.Poll(1, out).state, PollState::kEvent);
  EXPECT_EQ(out.sequence, 1U);
}

}  // namespace
}  // namespace yquant::marketd
