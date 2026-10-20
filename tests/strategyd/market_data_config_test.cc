#include "strategyd/market_data_config.h"

#include <cstdio>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "strategyd/json_config_document.h"
#include "test_config_file.h"

namespace yquant::strategyd {
namespace {

using testing::ConfigFile;

constexpr std::string_view kInstrument =
    R"({"venue":"SSE","symbol":"600000","price_decimals":2,)"
    R"("bid_price":1000,"bid_qty":100,"ask_price":1001,"ask_qty":200,"price_step":1})";

std::string Json(std::string_view instruments = kInstrument) {
  return R"({"source_id":1,"adapter":"mock","interval_ms":100,"instruments":[)" +
         std::string(instruments) + "]}";
}

std::string Replace(std::string json, std::string_view from,
                    std::string_view to) {
  const auto position = json.find(from);
  if (position != std::string::npos) json.replace(position, from.size(), to);
  return json;
}

TEST(MarketDataConfigTest, LoadsExactTypedValuesAndDecodedStrings) {
  ConfigFile file(Replace(Json(), "SSE", R"(\u0053SE)"));
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  const auto& config = loader.config();
  EXPECT_EQ(config.source_id.value, 1U);
  EXPECT_EQ(config.interval_ns, 100000000U);
  ASSERT_EQ(config.instrument_count, 1U);
  const auto& instrument = config.instruments[0];
  EXPECT_EQ(instrument.instrument.venue, model::Venue::kSse);
  EXPECT_EQ(instrument.instrument.symbol_view(), "600000");
  EXPECT_EQ(instrument.instrument.id.value, (1ULL << 32) | 600000);
  EXPECT_EQ(instrument.instrument.price_decimals, 2U);
  EXPECT_EQ(instrument.bid_price, 1000);
  EXPECT_EQ(instrument.ask_price, 1001);
  EXPECT_EQ(instrument.bid_qty, 100);
  EXPECT_EQ(instrument.ask_qty, 200);
  EXPECT_EQ(instrument.price_step, 1);
}

TEST(MarketDataConfigTest, PreservesIntegersAboveDoubleExactRange) {
  auto json =
      Replace(Json(), "\"bid_price\":1000", "\"bid_price\":9007199254740993");
  json = Replace(json, "\"ask_price\":1001", "\"ask_price\":9007199254740994");
  ConfigFile file(json);
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().instruments[0].bid_price, 9007199254740993LL);
}

struct InvalidField {
  const char* name;
  const char* from;
  const char* to;
};
class InvalidMarketConfigTest : public ::testing::TestWithParam<InvalidField> {
};

TEST_P(InvalidMarketConfigTest, RejectsInvalidTypesRangesAndUnknownFields) {
  const auto& item = GetParam();
  ConfigFile file(Replace(Json(), item.from, item.to));
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
  EXPECT_EQ(loader.config().instrument_count, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    Schema, InvalidMarketConfigTest,
    ::testing::Values(
        InvalidField{"SourceZero", "\"source_id\":1", "\"source_id\":0"},
        InvalidField{"SourceNegative", "\"source_id\":1", "\"source_id\":-1"},
        InvalidField{"SourceOverflow", "\"source_id\":1",
                     "\"source_id\":4294967296"},
        InvalidField{"SourceFraction", "\"source_id\":1", "\"source_id\":1.0"},
        InvalidField{"SourceString", "\"source_id\":1", "\"source_id\":\"1\""},
        InvalidField{"Adapter", "\"mock\"", "\"other\""},
        InvalidField{"IntervalZero", "\"interval_ms\":100",
                     "\"interval_ms\":0"},
        InvalidField{"IntervalLarge", "\"interval_ms\":100",
                     "\"interval_ms\":60001"},
        InvalidField{"IntervalExponent", "\"interval_ms\":100",
                     "\"interval_ms\":1e2"},
        InvalidField{"Venue", "\"SSE\"", "\"XXX\""},
        InvalidField{"SymbolLength", "\"600000\"", "\"00000\""},
        InvalidField{"SymbolDigit", "\"600000\"", "\"60000a\""},
        InvalidField{"SymbolType", "\"600000\"", "600000"},
        InvalidField{"Decimals", "\"price_decimals\":2",
                     "\"price_decimals\":7"},
        InvalidField{"NegativeDecimals", "\"price_decimals\":2",
                     "\"price_decimals\":-1"},
        InvalidField{"BidZero", "\"bid_price\":1000", "\"bid_price\":0"},
        InvalidField{"BidOverflow", "\"bid_price\":1000",
                     "\"bid_price\":9223372036854775808"},
        InvalidField{"BidFraction", "\"bid_price\":1000", "\"bid_price\":1.5"},
        InvalidField{"AskLocked", "\"ask_price\":1001", "\"ask_price\":1000"},
        InvalidField{"AskCrossed", "\"ask_price\":1001", "\"ask_price\":999"},
        InvalidField{"BidQtyZero", "\"bid_qty\":100", "\"bid_qty\":0"},
        InvalidField{"AskQtyNegative", "\"ask_qty\":200", "\"ask_qty\":-1"},
        InvalidField{"NegativeStep", "\"price_step\":1", "\"price_step\":-1"},
        InvalidField{"StepOverflow", "\"price_step\":1",
                     "\"price_step\":9223372036854775807"},
        InvalidField{"UnknownRoot", "\"source_id\":1",
                     "\"unknown\":0,\"source_id\":1"},
        InvalidField{"UnknownInstrument",
                     "\"venue\":", "\"unknown\":0,\"venue\":"},
        InvalidField{"MissingSource", "\"source_id\":1,", ""},
        InvalidField{"MissingAdapter", "\"adapter\":\"mock\",", ""},
        InvalidField{"MissingInterval", "\"interval_ms\":100,", ""},
        InvalidField{"MissingInstrumentField", "\"bid_qty\":100,", ""},
        InvalidField{"DuplicateRoot", "\"source_id\":1",
                     "\"source_id\":1,\"\\u0073ource_id\":1"},
        InvalidField{"DuplicateInstrument", "\"bid_qty\":100",
                     "\"bid_qty\":100,\"\\u0062id_qty\":100"}),
    [](const ::testing::TestParamInfo<InvalidField>& info) {
      return info.param.name;
    });

TEST(MarketDataConfigTest, RequiresAnInstrumentArrayWithOneTo64DistinctItems) {
  for (std::size_t count : {0U, 64U, 65U}) {
    std::string items;
    for (std::size_t i = 0; i < count; ++i) {
      if (i != 0) items += ',';
      char symbol[7];
      std::snprintf(symbol, sizeof(symbol), "%06zu", i);
      items += Replace(std::string(kInstrument), "600000", symbol);
    }
    ConfigFile file(Json(items));
    ASSERT_TRUE(file.valid());
    MarketDataConfigLoader loader;
    EXPECT_EQ(loader.Load(file.path()).ok(), count == 64)
        << loader.error().message;
  }
  ConfigFile duplicate(
      Json(std::string(kInstrument) + ',' + std::string(kInstrument)));
  ASSERT_TRUE(duplicate.valid());
  MarketDataConfigLoader loader;
  EXPECT_FALSE(loader.Load(duplicate.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "instruments[1].symbol");
  ConfigFile distinct(Json(std::string(kInstrument) + ',' +
                           Replace(std::string(kInstrument), "SSE", "SZSE")));
  ASSERT_TRUE(distinct.valid());
  EXPECT_TRUE(loader.Load(distinct.path()).ok()) << loader.error().message;
}

TEST(MarketDataConfigTest,
     ValidatesAllJsonBeforeSchemaAndReleasesTemporaryData) {
  MarketDataConfigLoader loader;
  std::string json = Json();
  json.resize(kMaxConfigFileBytes, ' ');
  ConfigFile full(json);
  ASSERT_TRUE(full.valid());
  ASSERT_TRUE(loader.Load(full.path()).ok()) << loader.error().message;
  json += ' ';
  ConfigFile large(json);
  ASSERT_TRUE(large.valid());
  EXPECT_FALSE(loader.Load(large.path()).ok());
  EXPECT_EQ(loader.config().instrument_count, 0U);
  std::string invalid_utf8 = Replace(Json(), "mock", std::string(1, '\x80'));
  ConfigFile utf8(invalid_utf8);
  ASSERT_TRUE(utf8.valid());
  EXPECT_FALSE(loader.Load(utf8.path()).ok());
  ConfigFile valid(Json());
  ASSERT_TRUE(valid.valid());
  ASSERT_TRUE(loader.Load(valid.path()).ok()) << loader.error().message;
  EXPECT_STREQ(loader.error().message, "");
  EXPECT_EQ(loader.config().instruments[0].instrument.symbol_view(), "600000");
}

}  // namespace
}  // namespace yquant::strategyd
