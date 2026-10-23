#include "yquant/trading/trading_config.h"

#include <stddef.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "gtest/gtest.h"
#include "test_config_file.h"
#include "yquant/base/format.h"
#include "yquant/base/json_config_document.h"
#include "yquant/model/identifiers.h"

namespace yquant::trading {
namespace {

using ::yquant::testing::ConfigFile;

constexpr std::string_view kJson =
    R"({"adapter":"mock","mode":"fill","report_interval_ms":1,)"
    R"("shutdown_timeout_ms":5000,"initial_cash":100000000000000,)"
    R"("initial_positions":[]})";

// Replaces one complete token fragment in a small test fixture.
std::string Replace(std::string_view from, std::string_view to) {
  std::string json(kJson);
  const auto offset = json.find(from);
  if (offset != std::string::npos) {
    json.replace(offset, from.size(), to);
  }
  return json;
}

TEST(TradingConfigTest, LoadsEveryModeAndExactDurationUnits) {
  struct ModeCase {
    const char* text;
    trading::MockTradingMode mode;
  };
  for (const auto item :
       {ModeCase{"fill", trading::MockTradingMode::kFill},
        ModeCase{"working", trading::MockTradingMode::kWorking},
        ModeCase{"reject", trading::MockTradingMode::kReject}}) {
    ConfigFile file(Replace("\"fill\"", '"' + std::string(item.text) + '"'));
    ASSERT_TRUE(file.valid());
    trading::TradingConfigLoader loader;
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
    EXPECT_EQ(loader.config().mode, item.mode);
    EXPECT_EQ(loader.config().report_interval_ns, 1000000U);
    EXPECT_EQ(loader.config().shutdown_timeout_ns, 5000000000ULL);
  }
}

TEST(TradingConfigTest, LoadsLocalCtpBridgeWithoutSyntheticAccount) {
  ConfigFile file(
      R"({"adapter":"ctp","address":"127.0.0.1","port":19001,)"
      R"("token":"local-session","connect_timeout_ms":60000,)"
      R"("heartbeat_timeout_ms":5000,"shutdown_timeout_ms":10000})");
  ASSERT_TRUE(file.valid());
  TradingConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, TradingAdapterKind::kCtp);
  EXPECT_EQ(loader.config().initial_account.type, model::AccountType::kFutures);
  EXPECT_EQ(loader.config().initial_account.cash_balance, 0);
  EXPECT_EQ(loader.config().ctp.port, 19001);
  EXPECT_STREQ(loader.config().ctp.token, "local-session");
  EXPECT_EQ(loader.config().ctp.connect_timeout_ns, 60000000000ULL);
}

TEST(TradingConfigTest, CtpRejectsRemoteBridgeAndSyntheticBalances) {
  const std::string valid =
      R"({"adapter":"ctp","address":"127.0.0.1","port":19001,)"
      R"("token":"local-session","connect_timeout_ms":60000,)"
      R"("heartbeat_timeout_ms":5000,"shutdown_timeout_ms":10000})";
  for (const auto& extra : {std::string(",\"initial_cash\":1000"),
                            std::string(",\"initial_futures_positions\":[]")}) {
    ConfigFile file(valid.substr(0, valid.size() - 1) + extra + "}");
    ASSERT_TRUE(file.valid());
    TradingConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok());
  }
  auto remote = valid;
  remote.replace(remote.find("127.0.0.1"), 9, "192.0.2.1");
  ConfigFile file(remote);
  TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "address");
}

struct BadField {
  const char* name;
  const char* before;
  const char* after;
  const char* expected_path = nullptr;
};
class BadTradingConfigTest : public ::testing::TestWithParam<BadField> {};

TEST_P(BadTradingConfigTest, RejectsInvalidSchemaAndPreservesDiagnostic) {
  const auto item = GetParam();
  ConfigFile file(Replace(item.before, item.after));
  ASSERT_TRUE(file.valid());
  trading::TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
  if (item.expected_path != nullptr) {
    EXPECT_STREQ(loader.error().field_path, item.expected_path);
  }
  EXPECT_EQ(loader.config().mode, trading::MockTradingMode::kFill);
  EXPECT_EQ(loader.config().report_interval_ns, 1000000U);
}

INSTANTIATE_TEST_SUITE_P(
    StrictSchema, BadTradingConfigTest,
    ::testing::Values(
        BadField{"MissingAdapter", "\"adapter\":\"mock\",", "", "adapter"},
        BadField{"MissingMode", "\"mode\":\"fill\",", "", "mode"},
        BadField{"MissingReportInterval", "\"report_interval_ms\":1,", "",
                 "report_interval_ms"},
        BadField{"MissingStopTimeout", ",\"shutdown_timeout_ms\":5000", "",
                 "shutdown_timeout_ms"},
        BadField{"MissingCash", ",\"initial_cash\":100000000000000", "",
                 "initial_cash"},
        BadField{"MissingPositions", ",\"initial_positions\":[]", "",
                 "initial_positions"},
        BadField{"NegativeCash", "100000000000000", "-1", "initial_cash"},
        BadField{"OverflowCash", "100000000000000", "9223372036854775808",
                 "initial_cash"},
        BadField{"FractionalCash", "100000000000000", "1.5", "initial_cash"},
        BadField{"StringCash", "100000000000000", "\"10\"", "initial_cash"},
        BadField{"NullPositions", "[]", "null", "initial_positions"},
        BadField{"WrongAdapter", "\"mock\"", "\"other\"", "adapter"},
        BadField{"WrongMode", "\"fill\"", "\"unknown\"", "mode"},
        BadField{"WrongModeType", "\"fill\"", "null", "mode"},
        BadField{"ZeroInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":0", "report_interval_ms"},
        BadField{"NegativeInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":-1", "report_interval_ms"},
        BadField{"TooLargeInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":60001", "report_interval_ms"},
        BadField{"FloatingInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":1.0", "report_interval_ms"},
        BadField{"ExponentInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":1e0", "report_interval_ms"},
        BadField{"StringInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":\"1\"", "report_interval_ms"},
        BadField{"OverflowInterval", "\"report_interval_ms\":1",
                 "\"report_interval_ms\":18446744073709551616",
                 "report_interval_ms"},
        BadField{"ZeroTimeout", "\"shutdown_timeout_ms\":5000",
                 "\"shutdown_timeout_ms\":0", "shutdown_timeout_ms"},
        BadField{"TimeoutLarge", "\"shutdown_timeout_ms\":5000",
                 "\"shutdown_timeout_ms\":60001", "shutdown_timeout_ms"},
        BadField{"TimeoutFraction", "\"shutdown_timeout_ms\":5000",
                 "\"shutdown_timeout_ms\":5000.5", "shutdown_timeout_ms"},
        BadField{"UnknownField",
                 "\"adapter\":", "\"unknown\":0,\"adapter\":", "unknown"},
        BadField{"DuplicateDecodedField", "\"mode\":\"fill\"",
                 "\"mode\":\"fill\",\"\\u006dode\":\"working\"", "mode"},
        BadField{"DuplicateNestedField", "\"mode\":\"fill\"",
                 "\"mode\":{\"x\":1,\"\\u0078\":2}", "mode.x"},
        BadField{"TrailingData", "[]}", "[]} {}"},
        BadField{"InvalidUnicode", "\"mock\"", "\"\\ud800\""}),
    [](const ::testing::TestParamInfo<BadField>& info) {
      return info.param.name;
    });

TEST(TradingConfigTest, DecodesFieldNamesAndAcceptsBothDurationBounds) {
  ConfigFile file(Replace("report_interval_ms", R"(report_interval_\u006ds)"));
  ASSERT_TRUE(file.valid());
  trading::TradingConfigLoader loader;
  EXPECT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  ConfigFile maximum(
      Replace("\"report_interval_ms\":1", "\"report_interval_ms\":60000"));
  ASSERT_TRUE(maximum.valid());
  ASSERT_TRUE(loader.Load(maximum.path()).ok());
  EXPECT_EQ(loader.config().report_interval_ns, 60000000000ULL);
  ConfigFile minimum(
      Replace("\"shutdown_timeout_ms\":5000", "\"shutdown_timeout_ms\":1"));
  ASSERT_TRUE(minimum.valid());
  ASSERT_TRUE(loader.Load(minimum.path()).ok());
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 1000000U);
}

TEST(TradingConfigTest,
     AppliesSharedFileLimitAndResetsPreviousValuesOnFailure) {
  trading::TradingConfigLoader loader;
  std::string large = Replace("\"fill\"", "\"working\"");
  large.resize(base::kMaxConfigFileBytes, ' ');
  ConfigFile full(large);
  ASSERT_TRUE(full.valid());
  ASSERT_TRUE(loader.Load(full.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().mode, trading::MockTradingMode::kWorking);
  large += ' ';
  ConfigFile excessive(large);
  ASSERT_TRUE(excessive.valid());
  EXPECT_FALSE(loader.Load(excessive.path()).ok());
  EXPECT_EQ(loader.config().mode, trading::MockTradingMode::kFill);
  EXPECT_FALSE(loader.Load("/yquant-missing-trading-config").ok());
  ConfigFile good(kJson);
  ASSERT_TRUE(good.valid());
  ASSERT_TRUE(loader.Load(good.path()).ok());
  EXPECT_STREQ(loader.error().message, "");
}

TEST(TradingConfigTest, TypedValidationRejectsUnknownModesAndBadDurations) {
  base::ConfigError error;
  trading::TradingConfig config;
  EXPECT_TRUE(trading::ValidateTradingConfig(config, error).ok());
  config.mode = static_cast<trading::MockTradingMode>(99);
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "mode");
  config = {};
  config.report_interval_ns = 0;
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "report_interval_ms");
  config = {};
  config.shutdown_timeout_ns = UINT64_MAX;
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "shutdown_timeout_ms");
}

constexpr std::string_view kPosition =
    R"({"venue":"SSE","symbol":"600000","total_qty":1000,"available_qty":800})";

// Modifies one position while retaining all newly required root fields.
std::string PositionJson(std::string_view before, std::string_view after) {
  std::string position(kPosition);
  const auto offset = position.find(before);
  if (offset != std::string::npos) {
    position.replace(offset, before.size(), after);
  }
  return Replace("[]", '[' + position + ']');
}

struct BadPosition {
  const char* name;
  const char* before;
  const char* after;
  const char* expected_path;
};
class BadInitialPositionTest : public ::testing::TestWithParam<BadPosition> {};

TEST_P(BadInitialPositionTest, RejectsInvalidPositionWithItsOwnFieldPath) {
  const auto item = GetParam();
  ConfigFile file(PositionJson(item.before, item.after));
  ASSERT_TRUE(file.valid());
  trading::TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_STREQ(loader.error().field_path, item.expected_path);
  EXPECT_EQ(loader.config().initial_account.position_count, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    AccountSchema, BadInitialPositionTest,
    ::testing::Values(
        BadPosition{"MissingVenue", "\"venue\":\"SSE\",", "",
                    "initial_positions[0].venue"},
        BadPosition{"MissingSymbol", ",\"symbol\":\"600000\"", "",
                    "initial_positions[0].symbol"},
        BadPosition{"MissingTotal", ",\"total_qty\":1000", "",
                    "initial_positions[0].total_qty"},
        BadPosition{"MissingAvailable", ",\"available_qty\":800", "",
                    "initial_positions[0].available_qty"},
        BadPosition{"InvalidVenue", "SSE", "XXX", "initial_positions[0].venue"},
        BadPosition{"InvalidSymbol", "600000", "60000a",
                    "initial_positions[0].symbol"},
        BadPosition{"SymbolNumber", "\"600000\"", "600000",
                    "initial_positions[0].symbol"},
        BadPosition{"NegativeTotal", "\"total_qty\":1000", "\"total_qty\":-1",
                    "initial_positions[0].total_qty"},
        BadPosition{"NegativeAvailable", "\"available_qty\":800",
                    "\"available_qty\":-1",
                    "initial_positions[0].available_qty"},
        BadPosition{"AvailableAboveTotal", "\"available_qty\":800",
                    "\"available_qty\":1001",
                    "initial_positions[0].available_qty"},
        BadPosition{"TotalOverflow", "\"total_qty\":1000",
                    "\"total_qty\":9223372036854775808",
                    "initial_positions[0].total_qty"},
        BadPosition{"AvailableOverflow", "\"available_qty\":800",
                    "\"available_qty\":9223372036854775808",
                    "initial_positions[0].available_qty"},
        BadPosition{"FractionalQuantity", "\"available_qty\":800",
                    "\"available_qty\":800.0",
                    "initial_positions[0].available_qty"},
        BadPosition{"StringQuantity", "\"total_qty\":1000",
                    "\"total_qty\":\"1000\"", "initial_positions[0].total_qty"},
        BadPosition{"ReservedFieldNotAllowed",
                    "\"venue\":", "\"reserved_qty\":0,\"venue\":",
                    "initial_positions[0].reserved_qty"},
        BadPosition{"DuplicateDecodedField", "\"total_qty\":1000",
                    "\"total_qty\":1000,\"\\u0074otal_qty\":1000",
                    "initial_positions[0].total_qty"}),
    [](const ::testing::TestParamInfo<BadPosition>& info) {
      return info.param.name;
    });

TEST(TradingConfigTest, ParsesInitialAccountWithoutFloatingPointOrBorrowing) {
  trading::TradingConfigLoader loader;
  {
    ConfigFile file(PositionJson("1000", "9223372036854775807"));
    ASSERT_TRUE(file.valid());
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  }
  const auto& account = loader.config().initial_account;
  EXPECT_EQ(account.cash_balance, 100000000000000LL);
  ASSERT_EQ(account.position_count, 1U);
  EXPECT_EQ(account.positions[0].instrument_id.value, (1ULL << 32) | 600000);
  EXPECT_EQ(account.positions[0].total_qty, INT64_MAX);
  EXPECT_EQ(account.positions[0].available_qty, 800);
  EXPECT_EQ(account.positions[0].reserved_qty, 0);
  ConfigFile exact(Replace("100000000000000", "9007199254740993"));
  ASSERT_TRUE(exact.valid());
  ASSERT_TRUE(loader.Load(exact.path()).ok());
  EXPECT_EQ(loader.config().initial_account.cash_balance, 9007199254740993LL);
  ConfigFile zero(Replace("100000000000000", "0"));
  ASSERT_TRUE(zero.valid());
  EXPECT_TRUE(loader.Load(zero.path()).ok());
}

TEST(TradingConfigTest, PositionCapacityAndVenueScopedIdentityAreValidated) {
  for (const size_t count : {64U, 65U}) {
    std::string positions = "[";
    for (size_t index = 0; index < count; ++index) {
      if (index != 0) {
        positions += ',';
      }
      char symbol[model::kSymbolLength + 1];
      ::yquant::base::FormatTo(symbol, sizeof(symbol), "{:0{}}", index,
                               static_cast<int>(model::kSymbolLength));
      std::string item(kPosition);
      item.replace(item.find("600000"), model::kSymbolLength, symbol);
      positions += item;
    }
    positions += ']';
    ConfigFile file(Replace("[]", positions));
    ASSERT_TRUE(file.valid());
    trading::TradingConfigLoader loader;
    EXPECT_EQ(loader.Load(file.path()).ok(), count == 64)
        << loader.error().message;
    if (count == 64) {
      EXPECT_EQ(loader.config().initial_account.position_count, 64U);
    }
  }
  trading::TradingConfigLoader loader;
  ConfigFile duplicate(Replace(
      "[]", '[' + std::string(kPosition) + ',' + std::string(kPosition) + ']'));
  ASSERT_TRUE(duplicate.valid());
  EXPECT_FALSE(loader.Load(duplicate.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "initial_positions[1].symbol");
  std::string other_venue(kPosition);
  other_venue.replace(other_venue.find("SSE"), 3, "SZSE");
  ConfigFile distinct(
      Replace("[]", '[' + std::string(kPosition) + ',' + other_venue + ']'));
  ASSERT_TRUE(distinct.valid());
  EXPECT_TRUE(loader.Load(distinct.path()).ok()) << loader.error().message;
  ConfigFile null_item(Replace("[]", "[null]"));
  ASSERT_TRUE(null_item.valid());
  EXPECT_FALSE(loader.Load(null_item.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "initial_positions[0]");
}

TEST(TradingConfigTest, TypedAccountValidationChecksReservationsAndBounds) {
  trading::TradingConfig config;
  base::ConfigError error;
  config.initial_account.cash_balance = -1;
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_cash");
  config.initial_account.cash_balance = INT64_MAX;
  config.initial_account.position_count = 65;
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions");
  config.initial_account.position_count = 1;
  auto& position = config.initial_account.positions[0];
  position.instrument_id = {(1ULL << 32) | 600000};
  position.total_qty = INT64_MAX;
  position.available_qty = INT64_MAX;
  EXPECT_TRUE(trading::ValidateTradingConfig(config, error).ok());
  position.reserved_qty = 1;
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions[0].reserved_qty");
  position.reserved_qty = 0;
  position.instrument_id = {1};
  EXPECT_FALSE(trading::ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions[0].symbol");
}

constexpr std::string_view kFuturesJson =
    R"({"adapter":"simulated_futures","report_interval_ms":1,)"
    R"("shutdown_timeout_ms":5000,"initial_cash":100000000000,)"
    R"("initial_futures_positions":[]})";

TEST(FuturesTradingConfigTest, FlatAccountNeedsNoInventoryOrMockMode) {
  ConfigFile file(kFuturesJson);
  TradingConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, TradingAdapterKind::kSimulatedFutures);
  EXPECT_EQ(loader.config().initial_account.type, model::AccountType::kFutures);
  EXPECT_EQ(loader.config().initial_account.cash_balance, 100000000000LL);
  EXPECT_EQ(loader.config().initial_account.futures_position_count, 0U);
}

// Four buckets preserve long/short and today/yesterday independently.
std::string FuturesPositionJson() {
  std::string json(kFuturesJson);
  json.replace(json.find("[]"), 2,
               R"([{"venue":"SHFE","symbol":"rb2701","reference_price":3200,)"
               R"("long_today":{"quantity":1,"open_price":3190},)"
               R"("long_yesterday":{"quantity":2,"open_price":3180},)"
               R"("short_today":{"quantity":3,"open_price":3220},)"
               R"("short_yesterday":{"quantity":0,"open_price":0}}])");
  return json;
}

TEST(FuturesTradingConfigTest, LoadsIndependentBucketsAndOwnsValues) {
  TradingConfigLoader loader;
  {
    ConfigFile file(FuturesPositionJson());
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  }
  const auto& account = loader.config().initial_account;
  ASSERT_EQ(account.futures_position_count, 1U);
  const auto& position = account.futures_positions[0];
  EXPECT_EQ(position.long_today.quantity, 1);
  EXPECT_EQ(position.long_yesterday.quantity, 2);
  EXPECT_EQ(position.short_today.open_price, 3220);
  EXPECT_EQ(position.short_yesterday.open_price, 0);
  EXPECT_EQ(position.reference_price, 3200);
  EXPECT_TRUE(model::IsValidInstrumentId(position.instrument_id));
}

TEST(FuturesTradingConfigTest, RejectsMixedMissingAndMalformedSchema) {
  for (const auto& tokens :
       {std::pair{"\"initial_futures_positions\":[]",
                  "\"initial_positions\":[]"},
        std::pair{"\"initial_futures_positions\":[]",
                  "\"initial_futures_positions\":[],\"mode\":\"fill\""},
        std::pair{"\"initial_futures_positions\":[]",
                  "\"initial_futures_positions\":null"}}) {
    std::string json(kFuturesJson);
    json.replace(json.find(tokens.first), std::string_view(tokens.first).size(),
                 tokens.second);
    ConfigFile file(json);
    TradingConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok());
    EXPECT_EQ(loader.config().adapter, TradingAdapterKind::kMock);
  }
  for (const auto& tokens :
       {std::pair{"\"quantity\":1", "\"quantity\":-1"},
        std::pair{"\"open_price\":3190", "\"open_price\":0"},
        std::pair{"\"reference_price\":3200", "\"reference_price\":0"},
        std::pair{"\"symbol\":\"rb2701\"", "\"symbol\":\"RB2701\""},
        std::pair{"\"quantity\":1", "\"quantity\":1.0"},
        std::pair{"\"quantity\":1", "\"quantity\":1,\"unknown\":0"}}) {
    std::string json = FuturesPositionJson();
    json.replace(json.find(tokens.first), std::string_view(tokens.first).size(),
                 tokens.second);
    ConfigFile file(json);
    TradingConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok()) << json;
  }
}

TEST(FuturesTradingConfigTest, TypedValidationChecksAdapterAndGrossOverflow) {
  TradingConfig config;
  base::ConfigError error;
  config.adapter = TradingAdapterKind::kSimulatedFutures;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  config.initial_account.type = model::AccountType::kFutures;
  EXPECT_TRUE(ValidateTradingConfig(config, error).ok());
  config.initial_account.futures_position_count = 1;
  auto& position = config.initial_account.futures_positions[0];
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", position.instrument_id),
            base::ErrorCode::kOk);
  position.reference_price = 3200;
  position.long_today = {INT64_MAX, 3200};
  position.short_today = {1, 3200};
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  position.short_today = {};
  EXPECT_TRUE(ValidateTradingConfig(config, error).ok());
  config.adapter = static_cast<TradingAdapterKind>(9);
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
}

TEST(FuturesTradingConfigTest,
     RequiresCompleteTypedPositionAndRejectsDuplicates) {
  for (const auto& removed :
       {std::string("\"adapter\":\"simulated_futures\","),
        std::string("\"report_interval_ms\":1,"),
        std::string("\"shutdown_timeout_ms\":5000,"),
        std::string("\"initial_cash\":100000000000,"),
        std::string(",\"initial_futures_positions\":[]")}) {
    std::string json(kFuturesJson);
    json.erase(json.find(removed), removed.size());
    ConfigFile file(json);
    TradingConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok()) << json;
  }
  auto json = FuturesPositionJson();
  const auto begin = json.find("[{"), end = json.find("}]") + 1;
  const auto position = json.substr(begin + 1, end - begin - 1);
  json.replace(begin, end - begin + 1, "[" + position + "," + position + "]");
  ConfigFile duplicate(json);
  TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(duplicate.path()).ok());
  EXPECT_STREQ(loader.error().field_path,
               "initial_futures_positions[1].symbol");
  json = FuturesPositionJson();
  const std::string bucket =
      "\"short_yesterday\":{\"quantity\":0,\"open_price\":0}";
  const auto offset = json.find(bucket);
  json.erase(offset - 1, bucket.size() + 1);
  ConfigFile missing(json);
  EXPECT_FALSE(loader.Load(missing.path()).ok());
  EXPECT_STREQ(loader.error().field_path,
               "initial_futures_positions[0].short_yesterday");
}

TEST(FuturesTradingConfigTest, HonorsPositionCapacityWithoutAliasingContracts) {
  auto json = FuturesPositionJson();
  const auto begin = json.find("[{"), end = json.find("}]") + 1;
  const auto position = json.substr(begin + 1, end - begin - 1);
  for (const size_t count : {64U, 65U}) {
    std::string positions = "[";
    for (size_t index = 0; index < count; ++index) {
      if (index) {
        positions += ',';
      }
      auto entry = position;
      char symbol[8];
      ::yquant::base::FormatTo(symbol, sizeof(symbol), "rb{:02}{:02}",
                               index / 12, index % 12 + 1);
      entry.replace(entry.find("rb2701"), 6, symbol);
      positions += entry;
    }
    positions += ']';
    auto input = json;
    input.replace(begin, end - begin + 1, positions);
    ConfigFile file(input);
    TradingConfigLoader loader;
    EXPECT_EQ(loader.Load(file.path()).ok(), count == 64)
        << loader.error().message;
  }
}

TEST(FuturesTradingConfigTest, MarksTruncatedUnknownPositionAndBucketPaths) {
  for (const bool nested : {false, true}) {
    auto json = FuturesPositionJson();
    const std::string key(400, 'x');
    const std::string marker =
        nested ? "\"quantity\":1" : "\"reference_price\":3200";
    json.insert(json.find(marker), "\"" + key + "\":0,");
    ConfigFile file(json);
    TradingConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok());
    EXPECT_TRUE(loader.error().truncated);
    EXPECT_NE(std::string_view(loader.error().field_path)
                  .find("initial_futures_positions[0]."),
              std::string_view::npos);
  }
}

}  // namespace
}  // namespace yquant::trading
