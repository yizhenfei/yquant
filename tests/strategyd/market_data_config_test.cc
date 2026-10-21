#include "strategyd/market_data_config.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#include "gtest/gtest.h"
#include "scoped_working_directory.h"
#include "yquant/base/json_config_document.h"
#include "test_config_file.h"
#include "yquant/model/identifiers.h"

namespace yquant::strategyd {
namespace {

using testing::ConfigFile;

constexpr std::string_view kInstrument =
    R"({"venue":"SSE","symbol":"600000",)"
    R"("bid_price":1000,"bid_qty":100,"ask_price":1001,)"
    R"("ask_qty":200,"price_step":1})";

std::string Json(
    std::string_view instruments = kInstrument,
    std::string_view catalog = YQUANT_TEST_INSTRUMENT_CATALOG_PATH) {
  return R"({"instrument_catalog":")" + std::string(catalog) +
         R"(","source_id":1,"adapter":"mock","interval_ms":100,)"
         R"("instruments":[)" +
         std::string(instruments) + "]}";
}

std::string Replace(std::string json, std::string_view from,
                    std::string_view to) {
  const auto position = json.find(from);
  if (position != std::string::npos) json.replace(position, from.size(), to);
  return json;
}

// Holds explicitly written host configurations and their shared catalog.
class CatalogConfigDirectory {
 public:
  CatalogConfigDirectory() { valid_ = mkdtemp(path_) != nullptr; }
  ~CatalogConfigDirectory() {
    if (valid_) {
      std::error_code error;
      std::filesystem::remove_all(path_, error);
    }
  }
  CatalogConfigDirectory(const CatalogConfigDirectory&) = delete;
  CatalogConfigDirectory& operator=(const CatalogConfigDirectory&) = delete;

  bool valid() const { return valid_; }
  std::string File(std::string_view name) const {
    return (std::filesystem::path(path_) / name).string();
  }
  bool Write(std::string_view name, std::string_view contents) const {
    if (!valid_) return false;
    const auto file_path = std::filesystem::path(path_) / name;
    std::error_code error;
    std::filesystem::create_directories(file_path.parent_path(), error);
    if (error) return false;
    FILE* file = std::fopen(file_path.c_str(), "wb");
    if (file == nullptr) return false;
    const auto written = std::fwrite(contents.data(), 1, contents.size(), file);
    return std::fclose(file) == 0 && written == contents.size();
  }

 private:
  char path_[64] = "/tmp/yquant-catalog-config-XXXXXX";
  bool valid_ = false;
};

using ::yquant::testing::ScopedWorkingDirectory;

TEST(MarketDataConfigTest, LoadsExactTypedValuesAndDecodedStrings) {
  ConfigFile file(Replace(Json(), "\"SSE\"", R"("\u0053SE")"));
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

TEST(MarketDataConfigTest, RequiresAnExplicitNonemptyCatalogPath) {
  const std::string catalog_value =
      '"' + std::string(YQUANT_TEST_INSTRUMENT_CATALOG_PATH) + '"';
  MarketDataConfigLoader loader;
  for (const std::string_view invalid :
       {"null", "1", "true", R"("")", R"("bad\u0000path")"}) {
    SCOPED_TRACE(invalid);
    ConfigFile file(Replace(Json(), catalog_value, invalid));
    ASSERT_TRUE(file.valid());
    EXPECT_FALSE(loader.Load(file.path()).ok());
    EXPECT_EQ(loader.config().instrument_count, 0U);
    EXPECT_EQ(loader.config().source_id.value, 0U);
    EXPECT_NE(loader.error().message[0], '\0');
  }
  ConfigFile missing(
      Replace(Json(), "\"instrument_catalog\":" + catalog_value + ',', ""));
  ASSERT_TRUE(missing.valid());
  EXPECT_FALSE(loader.Load(missing.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "instrument_catalog");
}

TEST(MarketDataConfigTest, RejectsCatalogPathThatOverflowsAfterResolution) {
  // The JSON reference fits its buffer; prepending cwd exceeds the limit.
  ConfigFile valid(Json());
  ConfigFile relative(Json(kInstrument, std::string(4095, 'x')));
  ASSERT_TRUE(valid.valid());
  ASSERT_TRUE(relative.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(valid.path()).ok());
  EXPECT_EQ(loader.Load(relative.path()).code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_STREQ(loader.error().field_path, "instrument_catalog");
  EXPECT_NE(loader.error().message[0], '\0');
  EXPECT_EQ(loader.config().instrument_count, 0U);
}

TEST(MarketDataConfigTest, FailedCatalogLoadClearsAnEarlierResolvedConfig) {
  constexpr std::string_view kCatalogs[] = {
      R"({"instruments":[{"venue":"SSE","symbol":"600000",)"
      R"("price_decimals":7}]})",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":2},)"
      R"({"venue":"SSE","symbol":"600000","price_decimals":4}]})",
      R"({"instruments":[]})",
  };
  ConfigFile valid(Json());
  ASSERT_TRUE(valid.valid());
  MarketDataConfigLoader loader;
  for (const auto catalog_json : kCatalogs) {
    ConfigFile catalog(catalog_json);
    ASSERT_TRUE(catalog.valid());
    ConfigFile host(Json(kInstrument, catalog.path()));
    ASSERT_TRUE(host.valid());
    ASSERT_TRUE(loader.Load(valid.path()).ok()) << loader.error().message;
    EXPECT_FALSE(loader.Load(host.path()).ok());
    EXPECT_EQ(loader.config().source_id.value, 0U);
    EXPECT_EQ(loader.config().instrument_count, 0U);
    EXPECT_EQ(loader.config().instruments[0].instrument.id.value, 0U);
    EXPECT_NE(loader.error().message[0], '\0');
  }
  ConfigFile missing(Json(kInstrument, "/yquant-no-such-catalog.json"));
  ASSERT_TRUE(missing.valid());
  ASSERT_TRUE(loader.Load(valid.path()).ok());
  EXPECT_FALSE(loader.Load(missing.path()).ok());
  EXPECT_EQ(loader.config().source_id.value, 0U);
  EXPECT_EQ(loader.config().instrument_count, 0U);
  ASSERT_TRUE(loader.Load(valid.path()).ok()) << loader.error().message;
  EXPECT_STREQ(loader.error().message, "");
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
        InvalidField{"InlineDecimals", "\"symbol\":\"600000\"",
                     "\"symbol\":\"600000\",\"price_decimals\":2"},
        InvalidField{"InlineDecimalsOverride", "\"symbol\":\"600000\"",
                     "\"symbol\":\"600000\",\"price_decimals\":4"},
        InvalidField{"MissingDefinition", "\"600000\"", "\"600002\""},
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
      char symbol[model::kSymbolLength + 1];
      std::snprintf(symbol, sizeof(symbol), "%0*zu",
                    static_cast<int>(model::kSymbolLength), i);
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
  json.resize(base::kMaxConfigFileBytes, ' ');
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

std::string ShmJson(
    std::string_view catalog = YQUANT_TEST_INSTRUMENT_CATALOG_PATH) {
  // Place instruments before adapter to exercise order-independent schema.
  return R"({"source_id":1,"instruments":[)"
         R"({"venue":"SSE","symbol":"600000"}],)"
         R"("runtime_directory":"/tmp/yquant","stream":"quotes.main",)"
         R"("adapter":"shm","instrument_catalog":")" +
         std::string(catalog) + "\"}";
}

TEST(MarketDataConfigTest,
     ResolvesNestedHostReferencesAgainstWorkingDirectory) {
  CatalogConfigDirectory directory;
  ASSERT_TRUE(directory.valid());
  ASSERT_TRUE(directory.Write(
      "catalog/instruments.json",
      R"({"instruments":[{"venue":"SSE","symbol":"600000","price_decimals":4},)"
      R"({"venue":"SZSE","symbol":"000001","price_decimals":3}]})"));
  ASSERT_TRUE(
      directory.Write("hosts/nested/mock.json",
                      Json(kInstrument, "../catalog/instruments.json")));
  ASSERT_TRUE(directory.Write("hosts/shm.json",
                              ShmJson("../catalog/instruments.json")));
  ASSERT_TRUE(directory.Write("unrelated/marker", ""));
  ScopedWorkingDirectory working_directory(directory.File("unrelated").c_str());
  ASSERT_TRUE(working_directory.valid());

  MarketDataConfigLoader mock;
  MarketDataConfigLoader shm;
  ASSERT_TRUE(mock.Load(directory.File("hosts/nested/mock.json").c_str()).ok())
      << mock.error().message;
  ASSERT_TRUE(shm.Load("../hosts/shm.json").ok()) << shm.error().message;
  ASSERT_EQ(mock.config().instrument_count, 1U);
  ASSERT_EQ(shm.config().instrument_count, 1U);
  const auto definition = mock.config().instruments[0].instrument;
  EXPECT_EQ(definition.price_decimals, 4U);
  EXPECT_EQ(shm.config().instruments[0].instrument, definition);

  std::error_code error;
  ASSERT_TRUE(std::filesystem::remove(
      directory.File("catalog/instruments.json"), error));
  ASSERT_FALSE(error);
  EXPECT_EQ(mock.config().instruments[0].instrument, definition);
  EXPECT_EQ(shm.config().instruments[0].instrument, definition);
}

TEST(MarketDataConfigTest, LoadsShmDictionaryAndOptionalPollInterval) {
  ConfigFile file(ShmJson());
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, MarketDataAdapter::kShm);
  EXPECT_STREQ(loader.config().runtime_directory, "/tmp/yquant");
  EXPECT_STREQ(loader.config().stream, "quotes.main");
  EXPECT_EQ(loader.config().poll_interval_ns, 0U);
  EXPECT_EQ(loader.config().interval_ns, 0U);
  ASSERT_EQ(loader.config().instrument_count, 1U);
  EXPECT_EQ(loader.config().instruments[0].instrument.symbol_view(), "600000");
  EXPECT_EQ(loader.config().instruments[0].bid_price, 0);
  ConfigFile interval(Replace(ShmJson(), "\"adapter\":\"shm\"",
                              "\"adapter\":\"shm\",\"poll_interval_us\":25"));
  ASSERT_TRUE(interval.valid());
  ASSERT_TRUE(loader.Load(interval.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().poll_interval_ns, 25000U);
}

TEST(MarketDataConfigTest, RejectsInvalidShmSchemaAndMixedAdapterFields) {
  constexpr InvalidField kCases[] = {
      {"RelativePath", "/tmp/yquant", "tmp/yquant"},
      {"ParentPath", "/tmp/yquant", "/tmp/../yquant"},
      {"NulPath", "/tmp/yquant", R"(/tmp/yquant\u0000other)"},
      {"InvalidStream", "quotes.main", "quotes/main"},
      {"StreamPrefix", "quotes.main", ".quotes"},
      {"EmptyStream", "quotes.main", ""},
      {"MissingStream", "\"stream\":\"quotes.main\",", ""},
      {"MissingDirectory", "\"runtime_directory\":\"/tmp/yquant\",", ""},
      {"MockInterval", "\"adapter\":\"shm\"",
       "\"adapter\":\"shm\",\"interval_ms\":1"},
      {"InlineDecimals", "\"symbol\":\"600000\"",
       "\"symbol\":\"600000\",\"price_decimals\":2"},
      {"MockPrice", "\"symbol\":\"600000\"",
       "\"symbol\":\"600000\",\"bid_price\":0"},
      {"PollNegative", "\"adapter\":\"shm\"",
       "\"adapter\":\"shm\",\"poll_interval_us\":-1"},
      {"PollOverflow", "\"adapter\":\"shm\"",
       "\"adapter\":\"shm\",\"poll_interval_us\":60000001"},
      {"PollType", "\"adapter\":\"shm\"",
       "\"adapter\":\"shm\",\"poll_interval_us\":1.5"},
  };
  MarketDataConfigLoader loader;
  for (const auto& item : kCases) {
    SCOPED_TRACE(item.name);
    ConfigFile file(Replace(ShmJson(), item.from, item.to));
    ASSERT_TRUE(file.valid());
    EXPECT_FALSE(loader.Load(file.path()).ok());
    EXPECT_NE(loader.error().message[0], '\0');
    EXPECT_EQ(loader.config().instrument_count, 0U);
  }
  ConfigFile mixed(Replace(Json(), "\"adapter\":\"mock\"",
                           "\"adapter\":\"mock\",\"poll_interval_us\":0"));
  ASSERT_TRUE(mixed.valid());
  EXPECT_FALSE(loader.Load(mixed.path()).ok());
}

}  // namespace
}  // namespace yquant::strategyd
