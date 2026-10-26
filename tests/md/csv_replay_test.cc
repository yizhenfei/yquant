#include "yquant/md/csv_replay.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "md/pipeline_test_support.h"
#include "yquant/base/format.h"
#include "yquant/md/quote_codec.h"

namespace yquant::md {
namespace {

constexpr std::string_view kHeader =
    "Date,Time,SecurityID,SecurityIDSource,OfferPX1,BidPX1,OfferSize1,BidSize1";

class CsvReplayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.valid());
    ::yquant::base::FormatTo(config_.path, sizeof(config_.path), "{}",
                             directory_.File("quotes.csv").c_str());
    config_.format = ReplayFormat::kDolphinDbCsv;
    config_.source_id = {42};
    config_.max_records = 32;
    ::yquant::base::FormatTo(config_.instrument_catalog,
                             sizeof(config_.instrument_catalog), "{}",
                             directory_.File("instruments.json").c_str());
    ASSERT_TRUE(SetCatalog(4));
  }

  // Each fixture owns the definitions used to interpret its CSV prices.
  bool SetCatalog(uint8_t sse_decimals) {
    return directory_.Write(
        "instruments.json",
        R"({"instruments":[{"venue":"SSE","symbol":"600000",)"
        R"("price_decimals":)" +
            std::to_string(sse_decimals) +
            R"(},{"venue":"SZSE","symbol":"000519","price_decimals":4},)"
            R"({"venue":"SZSE","symbol":"000557","price_decimals":4},)"
            R"({"venue":"SZSE","symbol":"000895","price_decimals":4}]})");
  }

  base::Status Load(std::string_view csv) {
    EXPECT_TRUE(directory_.Write("quotes.csv", csv));
    return LoadDolphinDbCsv(config_, data_, detail_);
  }

  testing::RuntimeDirectory directory_;
  ReplayConfig config_;
  ReplayData data_;
  std::array<char, 256> detail_{};
};

TEST_F(CsvReplayTest, ConvertsRealSnapshotValuesAndCanonicalizesMissingSide) {
  // Values come from the pinned DolphinDB 2020-12-31 snapshot, including the
  // binary-floating-point export tail on the 000895 price.
  const auto csv = std::string(kHeader) +
                   "\n2020.12.31,09:15:00.000,000519,102 ,0,0,0,0\n"
                   "2020.12.31,09:15:00.000,000557,102 ,4.3,4.3,86200,86200\n"
                   "2020.12.31,09:15:00.000,000895,102 ,46.590000000000003,"
                   "46.590000000000003,700,700\n";
  ASSERT_TRUE(Load(csv).ok()) << detail_.data();
  ASSERT_EQ(data_.size(), 3U);
  const auto& absent = data_[0].quote;
  EXPECT_EQ(absent.source_id.value, 42U);
  EXPECT_EQ(absent.event_time_ns, 1609377300000000000ULL);
  EXPECT_EQ(absent.receive_time_ns, 0U);
  EXPECT_EQ(absent.bid_qty, 0);
  EXPECT_EQ(absent.ask_qty, 0);
  EXPECT_EQ(absent.flags,
            static_cast<uint8_t>(model::QuoteFlag::kEventTimeValid));
  EXPECT_EQ(data_[1].quote.bid_price, 43000);
  EXPECT_EQ(data_[1].quote.ask_qty, 86200);
  EXPECT_EQ(data_[2].quote.bid_price, 465900);
  EXPECT_FALSE(
      model::HasQuoteFlag(data_[2].quote.flags, model::QuoteFlag::kTradable));
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SZSE", "000557", id),
            base::ErrorCode::kOk);
  EXPECT_EQ(data_[1].quote.instrument_id, id);
  for (size_t i = 0; i < data_.size(); ++i) {
    EXPECT_TRUE(md::ValidateQuote(data_[i].quote).ok());
  }
}

TEST_F(CsvReplayTest, RoundsPricesUsingExactDecimalText) {
  ASSERT_TRUE(SetCatalog(2));
  ASSERT_TRUE(
      Load(std::string(kHeader) +
           "\n2020.12.31,09:15:00.000,600000,101,1.005,1.00499999999,1,1\n"
           "2020.12.31,09:15:00.001,600000,101,2.99999999,4.309999999999999,"
           "1,1\n")
          .ok())
      << detail_.data();
  EXPECT_EQ(data_[0].quote.ask_price, 101);
  EXPECT_EQ(data_[0].quote.bid_price, 100);
  EXPECT_EQ(data_[1].quote.ask_price, 300);
  EXPECT_EQ(data_[1].quote.bid_price, 431);
  EXPECT_EQ(data_[1].quote.sequence, 2U);
  EXPECT_EQ(data_[1].offset_ns, 1000000U);
}

TEST_F(CsvReplayTest, UsesEachInstrumentsCatalogPricePrecision) {
  ASSERT_TRUE(SetCatalog(2));
  ASSERT_TRUE(Load(std::string(kHeader) +
                   "\n2020.12.31,09:15:00,600000,101,4.3,4.3,1,1\n"
                   "2020.12.31,09:15:00,000557,102,4.3,4.3,1,1\n")
                  .ok())
      << detail_.data();
  ASSERT_EQ(data_.size(), 2U);
  EXPECT_EQ(data_[0].quote.ask_price, 430);
  EXPECT_EQ(data_[0].quote.bid_price, 430);
  EXPECT_EQ(data_[1].quote.ask_price, 43000);
  EXPECT_EQ(data_[1].quote.bid_price, 43000);
}

TEST_F(CsvReplayTest, UnknownInstrumentRejectsWholeFileAndPreservesOutput) {
  const auto known = "2020.12.31,09:15:00,000557,102,4.3,4.3,1,1\n";
  ASSERT_TRUE(Load(std::string(kHeader) + "\n" + known).ok());
  const auto previous = data_[0].quote;
  EXPECT_EQ(Load(std::string(kHeader) + "\n" + known +
                 "2020.12.31,09:15:01,600001,101,4,4,1,1\n")
                .code,
            base::ErrorCode::kCorrupt);
  EXPECT_NE(std::strstr(detail_.data(), "line 3"), nullptr);
  EXPECT_NE(std::strstr(detail_.data(), "not defined in the catalog"), nullptr);
  ASSERT_EQ(data_.size(), 1U);
  EXPECT_EQ(data_[0].quote, previous);
}

TEST_F(CsvReplayTest, CatalogLoadFailurePreservesPreviousOutput) {
  const auto csv =
      std::string(kHeader) + "\n2020.12.31,09:15:00,000557,102,4,4,1,1\n";
  ASSERT_TRUE(Load(csv).ok());
  const auto previous = data_[0].quote;
  ASSERT_TRUE(SetCatalog(7));
  EXPECT_EQ(Load(csv).code, base::ErrorCode::kInvalidArgument);
  EXPECT_NE(std::strstr(detail_.data(), "instrument catalog"), nullptr);
  ASSERT_EQ(data_.size(), 1U);
  EXPECT_EQ(data_[0].quote, previous);
  ::yquant::base::FormatTo(config_.instrument_catalog,
                           sizeof(config_.instrument_catalog), "{}",
                           directory_.File("missing.json").c_str());
  EXPECT_EQ(Load(csv).code, base::ErrorCode::kIoError);
  ASSERT_EQ(data_.size(), 1U);
  EXPECT_EQ(data_[0].quote, previous);
}

TEST_F(CsvReplayTest, HandlesQuotedColumnsEscapesCrLfAndMultilineIgnoredField) {
  const auto csv =
      "\xef\xbb\xbf\"Ignored\",\"SecurityID\",\"Time\",\"Date\",\"BidSize1\","
      "\"BidPX1\",\"OfferSize1\",\"OfferPX1\",\"SecurityIDSource\"\r\n"
      "\"a comma, and \"\"quote\"\"\nsecond line\",\"600000\",\"09:15:00.000\","
      "\"2020.12.31\",\"7\",\"12.5\",\"0\",\"99\",\"101\"\r\n";
  ASSERT_TRUE(Load(csv).ok()) << detail_.data();
  ASSERT_EQ(data_.size(), 1U);
  EXPECT_EQ(data_[0].quote.bid_price, 125000);
  EXPECT_EQ(data_[0].quote.bid_qty, 7);
  EXPECT_EQ(data_[0].quote.ask_price, 0);
  EXPECT_EQ(data_[0].quote.ask_qty, 0);
  EXPECT_FALSE(
      model::HasQuoteFlag(data_[0].quote.flags, model::QuoteFlag::kAskValid));
}

TEST_F(CsvReplayTest, UsesReceiveTimesForPacingAndPreservesEventTime) {
  const auto csv = std::string(kHeader) +
                   ",RecvTime\n"
                   "2020.12.31,09:15:00.000,000557,102,4.3,4.3,10,10,"
                   "2020.12.31T09:15:00.748\n"
                   "2020.12.31,09:15:00.000,000557,102,4.3,4.3,10,10,"
                   "2020.12.31T09:15:00.800\n"
                   "2020.12.31,09:15:00.500,000557,102,4.3,4.3,10,10,"
                   "2020.12.31T09:15:00.700\n";
  ASSERT_TRUE(Load(csv).ok()) << detail_.data();
  ASSERT_EQ(data_.size(), 3U);
  EXPECT_EQ(data_[0].offset_ns, 0U);
  EXPECT_EQ(data_[1].offset_ns, 52000000U);
  EXPECT_EQ(data_[2].offset_ns, 52000000U);
  EXPECT_EQ(data_[0].quote.event_time_ns, data_[1].quote.event_time_ns);
  EXPECT_EQ(data_[2].quote.event_time_ns - data_[0].quote.event_time_ns,
            500000000U);
}

TEST_F(CsvReplayTest, PreservesFileOrderAndPerInstrumentSequences) {
  ASSERT_TRUE(Load(std::string(kHeader) +
                   "\n2020.12.31,09:15:01,000557,102,4,4,1,1\n"
                   "2020.12.31,09:15:02,000557,102,5,5,1,1\n"
                   "2020.12.31,09:15:00,600000,101,6,6,1,1\n"
                   "2020.12.31,09:15:01.500,000557,102,7,7,1,1\n")
                  .ok())
      << detail_.data();
  ASSERT_EQ(data_.size(), 4U);
  EXPECT_EQ(data_[0].quote.sequence, 1U);
  EXPECT_EQ(data_[1].quote.sequence, 2U);
  EXPECT_EQ(data_[2].quote.sequence, 1U);
  EXPECT_EQ(data_[3].quote.sequence, 3U);
  EXPECT_EQ(data_[1].offset_ns, 1000000000U);
  EXPECT_EQ(data_[2].offset_ns, 1000000000U);
  EXPECT_EQ(data_[3].offset_ns, 1000000000U);
  EXPECT_EQ(data_[3].quote.bid_price, 70000);
}

TEST_F(CsvReplayTest, AcceptsEmptyReplayWithHeaderAndUnsignedLimits) {
  ASSERT_TRUE(Load(std::string(kHeader) + "\n").ok());
  EXPECT_EQ(data_.size(), 0U);
  ASSERT_TRUE(SetCatalog(0));
  ASSERT_TRUE(Load(std::string(kHeader) +
                   "\n2020.02.29,08:00:00.123456789,600000,101,"
                   "9223372036854775807,0.5,9223372036854775807,1\n")
                  .ok())
      << detail_.data();
  ASSERT_EQ(data_.size(), 1U);
  EXPECT_EQ(data_[0].quote.ask_price, 9223372036854775807LL);
  EXPECT_EQ(data_[0].quote.ask_qty, 9223372036854775807LL);
  EXPECT_EQ(data_[0].quote.bid_price, 1);
  EXPECT_EQ(data_[0].quote.event_time_ns % 1000000000, 123456789U);
}

TEST_F(CsvReplayTest, RejectsInvalidDataAndPreservesPreviousOutput) {
  ASSERT_TRUE(Load(std::string(kHeader) +
                   "\n2020.12.31,09:15:00.000,000557,102,4,4,1,1\n")
                  .ok());
  const auto previous = data_[0].quote;
  const std::array<std::string_view, 15> bad_rows = {
      "2021.02.29,09:15:00,000557,102,4,4,1,1",
      "2020.12.31,24:15:00,000557,102,4,4,1,1",
      "2020.12.31,09:60:00,000557,102,4,4,1,1",
      "2020.12.31,09:15:60,000557,102,4,4,1,1",
      "2020.12.31,09:15:00.1234567890,000557,102,4,4,1,1",
      "2020.12.31,09:15:00,00055,102,4,4,1,1",
      "2020.12.31,09:15:00,000557,103,4,4,1,1",
      "2020.12.31,09:15:00,000557,102,-4,4,1,1",
      "2020.12.31,09:15:00,000557,102,nan,4,1,1",
      "2020.12.31,09:15:00,000557,102,1e9,4,1,1",
      "2020.12.31,09:15:00,000557,102,922337203685478,4,1,1",
      "2020.12.31,09:15:00,000557,102,4,4,-1,1",
      "2020.12.31,09:15:00,000557,102,4,4,1.5,1",
      "2020.12.31,09:15:00,000557,102,4,4,9223372036854775808,1",
      "1970.01.01,00:00:00,000557,102,4,4,1,1"};
  for (const auto row : bad_rows) {
    SCOPED_TRACE(row);
    const auto status =
        Load(std::string(kHeader) + "\n" + std::string(row) + "\n");
    EXPECT_EQ(status.code, base::ErrorCode::kCorrupt);
    EXPECT_NE(std::string_view(detail_.data()).find("line 2"),
              std::string_view::npos);
    ASSERT_EQ(data_.size(), 1U);
    EXPECT_EQ(data_[0].quote, previous);
  }
}

TEST_F(CsvReplayTest, RejectsMalformedCsvAndMissingColumns) {
  const std::array<std::string, 7> malformed = {
      "",
      std::string(kHeader) + ",Date\n",
      "Date,Time,SecurityID\n",
      std::string(kHeader) + "\n\"2020.12.31,09:15:00,000557,102,4,4,1,1\n",
      std::string(kHeader) + "\n2020.12.31,09:15:00,000557,102,4,4,1\n",
      std::string(kHeader) + "\n2020.12.31,09:15:00,000557,102,4\"x,4,1,1\n",
      std::string(kHeader) + "\n\"2020.12.31\"x,09:15:00,000557,102,4,4,1,1\n"};
  for (const auto& csv : malformed) {
    SCOPED_TRACE(csv);
    EXPECT_EQ(Load(csv).code, base::ErrorCode::kCorrupt);
  }
  EXPECT_EQ(Load(std::string(kHeader) + "\n" + std::string(65537, 'x')).code,
            base::ErrorCode::kCorrupt);
  std::string nul(kHeader);
  nul.push_back('\0');
  EXPECT_EQ(Load(nul).code, base::ErrorCode::kCorrupt);
}

TEST_F(CsvReplayTest, EnforcesCapacityAndValidatesOptionalReceiveTime) {
  config_.max_records = 1;
  EXPECT_EQ(
      Load(std::string(kHeader) + "\n2020.12.31,09:15:00,000557,102,4,4,1,1\n"
                                  "2020.12.31,09:15:01,000557,102,4,4,1,1\n")
          .code,
      base::ErrorCode::kResourceExhausted);
  EXPECT_NE(std::string_view(detail_.data()).find("line 3"),
            std::string_view::npos);
  EXPECT_EQ(Load(std::string(kHeader) +
                 ",RecvTime\n2020.12.31,09:15:00,000557,102,4,4,1,1,invalid\n")
                .code,
            base::ErrorCode::kCorrupt);
}

TEST_F(CsvReplayTest,
       ReportsOpenFailureAndInvalidConfigWithoutReplacingOutput) {
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, detail_).code,
            base::ErrorCode::kIoError);
  config_.source_id = {0};
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, {}).code,
            base::ErrorCode::kInvalidArgument);
  config_.source_id = {1};
  ::yquant::base::FormatTo(config_.instrument_catalog,
                           sizeof(config_.instrument_catalog), "{}",
                           "relative.json");
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, {}).code,
            base::ErrorCode::kInvalidArgument);
  config_.instrument_catalog[0] = '\0';
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, {}).code,
            base::ErrorCode::kInvalidArgument);
  std::memset(config_.instrument_catalog, 'x',
              sizeof(config_.instrument_catalog));
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, {}).code,
            base::ErrorCode::kInvalidArgument);
}

TEST_F(CsvReplayTest, RejectsNonRegularInputWithoutWaitingForPipeWriter) {
  ASSERT_EQ(::mkfifo(config_.path, 0600), 0);
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, detail_).code,
            base::ErrorCode::kInvalidArgument);
  ::yquant::base::FormatTo(config_.path, sizeof(config_.path), "{}",
                           directory_.path());
  EXPECT_EQ(LoadDolphinDbCsv(config_, data_, detail_).code,
            base::ErrorCode::kInvalidArgument);
}

}  // namespace
}  // namespace yquant::md
