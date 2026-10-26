#include "yquant/market/market_data_config.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "scoped_working_directory.h"
#include "test_config_file.h"
#include "yquant/base/json_config_document.h"
#include "yquant/model/identifiers.h"

namespace yquant::market {
namespace {

using ::yquant::testing::ConfigFile;

std::string Replace(std::string json, std::string_view from,
                    std::string_view to) {
  const auto position = json.find(from);
  if (position != std::string::npos) {
    json.replace(position, from.size(), to);
  }
  return json;
}

// Holds explicitly written host configurations and their shared catalog.
class CatalogConfigDirectory {
 public:
  CatalogConfigDirectory() { valid_ = mkdtemp(path_) != nullptr; }
  ~CatalogConfigDirectory() {
    if (valid_) {
      std::error_code error;
      ::yquant::testing::files::RemoveTree(
          ::yquant::testing::files::FilePath(path_), error);
    }
  }
  CatalogConfigDirectory(const CatalogConfigDirectory&) = delete;
  CatalogConfigDirectory& operator=(const CatalogConfigDirectory&) = delete;

  bool valid() const { return valid_; }
  std::string File(std::string_view name) const {
    return (::yquant::testing::files::FilePath(path_) / name).string();
  }
  bool Write(std::string_view name, std::string_view contents) const {
    if (!valid_) {
      return false;
    }
    const auto file_path = ::yquant::testing::files::FilePath(path_) / name;
    std::error_code error;
    ::yquant::testing::files::CreateDirectories(
        ::yquant::testing::files::FilePath(file_path.Parent()), error);
    if (error) {
      return false;
    }
    FILE* file = std::fopen(file_path.c_str(), "wb");
    if (file == nullptr) {
      return false;
    }
    const auto written = std::fwrite(contents.data(), 1, contents.size(), file);
    return std::fclose(file) == 0 && written == contents.size();
  }

 private:
  char path_[64] = "/tmp/yquant-catalog-config-XXXXXX";
  bool valid_ = false;
};

using ::yquant::testing::ScopedWorkingDirectory;

struct InvalidField {
  const char* name;
  const char* from;
  const char* to;
};
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
  ASSERT_TRUE(directory.Write("hosts/nested/mock.json",
                              ShmJson("../catalog/instruments.json")));
  ASSERT_TRUE(directory.Write("hosts/shm.json",
                              ShmJson("../catalog/instruments.json")));
  ASSERT_TRUE(directory.Write("unrelated/marker", ""));
  ScopedWorkingDirectory working_directory(directory.File("unrelated").c_str());
  ASSERT_TRUE(working_directory.valid());

  market::MarketDataConfigLoader mock;
  market::MarketDataConfigLoader shm;
  ASSERT_TRUE(mock.Load(directory.File("hosts/nested/mock.json").c_str()).ok())
      << mock.error().message;
  ASSERT_TRUE(shm.Load("../hosts/shm.json").ok()) << shm.error().message;
  ASSERT_EQ(mock.config().instrument_count, 1U);
  ASSERT_EQ(shm.config().instrument_count, 1U);
  const auto definition = mock.config().instruments[0].instrument;
  EXPECT_EQ(definition.price_decimals, 4U);
  EXPECT_EQ(shm.config().instruments[0].instrument, definition);

  std::error_code error;
  ASSERT_TRUE(::yquant::testing::files::RemoveFile(
      ::yquant::testing::files::FilePath(
          directory.File("catalog/instruments.json")),
      error));
  ASSERT_FALSE(error);
  EXPECT_EQ(mock.config().instruments[0].instrument, definition);
  EXPECT_EQ(shm.config().instruments[0].instrument, definition);
}

TEST(MarketDataConfigTest, LoadsShmDictionaryAndOptionalPollInterval) {
  ConfigFile file(ShmJson());
  ASSERT_TRUE(file.valid());
  market::MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, market::MarketDataAdapter::kShm);
  EXPECT_STREQ(loader.config().runtime_directory, "/tmp/yquant");
  EXPECT_STREQ(loader.config().stream, "quotes.main");
  EXPECT_EQ(loader.config().poll_interval_ns, 0U);
  ASSERT_EQ(loader.config().instrument_count, 1U);
  EXPECT_EQ(loader.config().instruments[0].instrument.symbol_view(), "600000");
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
  market::MarketDataConfigLoader loader;
  for (const auto& item : kCases) {
    SCOPED_TRACE(item.name);
    ConfigFile file(Replace(ShmJson(), item.from, item.to));
    ASSERT_TRUE(file.valid());
    EXPECT_FALSE(loader.Load(file.path()).ok());
    EXPECT_NE(loader.error().message[0], '\0');
    EXPECT_EQ(loader.config().instrument_count, 0U);
  }
  ConfigFile mixed(
      Replace(ShmJson(), "\"adapter\":\"shm\"", "\"adapter\":\"mock\""));
  ASSERT_TRUE(mixed.valid());
  EXPECT_FALSE(loader.Load(mixed.path()).ok());
}

}  // namespace
}  // namespace yquant::market
