#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#include "gtest/gtest.h"
#include "scoped_working_directory.h"
#include "test_config_file.h"
#include "yquant/base/json_config_document.h"
#include "yquant/md/market_data_config.h"
#include "yquant/model/identifiers.h"

namespace yquant::md {
namespace {

using ::yquant::testing::ConfigFile;

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

struct InvalidField {
  const char* name;
  std::string_view from;
  std::string_view to;
};
std::string FutureInstrument(bool) {
  return R"({"venue":"SHFE","symbol":"rb2501"})";
}
std::string ReplayJson(std::string_view extra = "") {
  return R"({"instrument_catalog":")" +
         std::string(YQUANT_TEST_INSTRUMENT_CATALOG_PATH) +
         R"(","source_id":1,"adapter":"replay","replay_path":"quotes.csv",)"
         R"("instruments":[)" +
         FutureInstrument(false) + "]" + std::string(extra) + '}';
}

TEST(MarketDataConfigTest,
     ResolvesReplayPathAgainstWorkingDirectoryAndDefaults) {
  ConfigFile file(ReplayJson());
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, MarketDataAdapter::kReplay);
  const std::string path(file.path());
  const auto expected =
      std::filesystem::current_path().string() + "/quotes.csv";
  EXPECT_STREQ(loader.config().replay_path, expected.c_str());
  EXPECT_EQ(loader.config().replay_speed, 1U);
  EXPECT_EQ(loader.config().replay_tail_ns, 1000000000U);
  ConfigFile absolute(Replace(ReplayJson(), "quotes.csv", "/tmp/futures.csv"));
  ASSERT_TRUE(absolute.valid());
  ASSERT_TRUE(loader.Load(absolute.path()).ok());
  EXPECT_STREQ(loader.config().replay_path, "/tmp/futures.csv");
}

TEST(MarketDataConfigTest, ParsesReplaySpeedAndTailBoundariesExactly) {
  for (const auto extra : {R"(,"replay_speed":1,"replay_tail_ms":0)",
                           R"(,"replay_speed":1000,"replay_tail_ms":60000)"}) {
    ConfigFile file(ReplayJson(extra));
    ASSERT_TRUE(file.valid());
    MarketDataConfigLoader loader;
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
    EXPECT_LE(loader.config().replay_speed, 1000U);
    EXPECT_LE(loader.config().replay_tail_ns, 60000000000ULL);
  }
}

class InvalidReplayMarketConfigTest
    : public ::testing::TestWithParam<InvalidField> {};

TEST_P(InvalidReplayMarketConfigTest, RejectsTransportAndTimingSchemaErrors) {
  const auto& item = GetParam();
  ConfigFile file(Replace(ReplayJson(), item.from, item.to));
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
  EXPECT_EQ(loader.config().instrument_count, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    ReplaySchema, InvalidReplayMarketConfigTest,
    ::testing::Values(
        InvalidField{"MissingPath", R"("replay_path":"quotes.csv",)", ""},
        InvalidField{"EmptyPath", "quotes.csv", ""},
        InvalidField{"NulPath", "quotes.csv", R"(quotes\u0000.csv)"},
        InvalidField{"SpeedZero", R"("source_id":1)",
                     R"("source_id":1,"replay_speed":0)"},
        InvalidField{"SpeedLarge", R"("source_id":1)",
                     R"("source_id":1,"replay_speed":1001)"},
        InvalidField{"SpeedFraction", R"("source_id":1)",
                     R"("source_id":1,"replay_speed":1.0)"},
        InvalidField{"SpeedString", R"("source_id":1)",
                     R"("source_id":1,"replay_speed":"1")"},
        InvalidField{"NegativeTail", R"("source_id":1)",
                     R"("source_id":1,"replay_tail_ms":-1)"},
        InvalidField{"LargeTail", R"("source_id":1)",
                     R"("source_id":1,"replay_tail_ms":60001)"},
        InvalidField{"MockInterval", R"("source_id":1)",
                     R"("source_id":1,"interval_ms":1)"},
        InvalidField{"ShmPoll", R"("source_id":1)",
                     R"("source_id":1,"poll_interval_us":0)"},
        InvalidField{"MockSeed", R"("venue":)", R"("bid_price":0,"venue":)"}),
    [](const ::testing::TestParamInfo<InvalidField>& info) {
      return info.param.name;
    });

TEST(MarketDataConfigTest,
     ReplayRejectsLongResolvedPathsAndMultipleInstruments) {
  ConfigFile long_path(
      Replace(ReplayJson(), "quotes.csv", std::string(4095, 'a')));
  ASSERT_TRUE(long_path.valid());
  MarketDataConfigLoader loader;
  EXPECT_FALSE(loader.Load(long_path.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "replay_path");
  auto json = ReplayJson();
  json = Replace(json, FutureInstrument(false),
                 FutureInstrument(false) + ',' +
                     Replace(FutureInstrument(false), "rb2501", "rb2502"));
  ConfigFile multiple(json);
  ASSERT_TRUE(multiple.valid());
  EXPECT_FALSE(loader.Load(multiple.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "instruments");
}

TEST(MarketDataConfigTest, TypedReplayValidationRejectsInvalidSettings) {
  ConfigFile file(ReplayJson());
  ASSERT_TRUE(file.valid());
  MarketDataConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok());
  const auto valid = loader.config();
  base::ConfigError error;
  auto candidate = valid;
  candidate.replay_path[0] = 'r';
  EXPECT_FALSE(ValidateMarketDataConfig(candidate, error).ok());
  EXPECT_STREQ(error.field_path, "replay_path");
  candidate = valid;
  std::memset(candidate.replay_path, 'x', sizeof(candidate.replay_path));
  EXPECT_FALSE(ValidateMarketDataConfig(candidate, error).ok());
  candidate = valid;
  candidate.replay_speed = 0;
  EXPECT_FALSE(ValidateMarketDataConfig(candidate, error).ok());
  EXPECT_STREQ(error.field_path, "replay_speed");
  candidate = valid;
  candidate.replay_tail_ns = 60000000001ULL;
  EXPECT_FALSE(ValidateMarketDataConfig(candidate, error).ok());
  EXPECT_STREQ(error.field_path, "replay_tail_ms");
  candidate = valid;
  candidate.instrument_count = 2;
  EXPECT_FALSE(ValidateMarketDataConfig(candidate, error).ok());
  EXPECT_STREQ(error.field_path, "instruments");
}

}  // namespace
}  // namespace yquant::md
