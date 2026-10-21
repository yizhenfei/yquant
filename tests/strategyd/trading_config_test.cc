#include "strategyd/trading_config.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "test_config_file.h"
#include "yquant/base/json_config_document.h"
#include "yquant/model/identifiers.h"

namespace yquant::strategyd {
namespace {

using testing::ConfigFile;

constexpr std::string_view kJson =
    R"({"adapter":"mock","mode":"fill","report_interval_ms":1,)"
    R"("shutdown_timeout_ms":5000,"initial_cash":100000000000000,)"
    R"("initial_positions":[]})";

// Replaces one complete token fragment in a small test fixture.
std::string Replace(std::string_view from, std::string_view to) {
  std::string json(kJson);
  const auto offset = json.find(from);
  if (offset != std::string::npos) json.replace(offset, from.size(), to);
  return json;
}

TEST(TradingConfigTest, LoadsEveryModeAndExactDurationUnits) {
  struct ModeCase {
    const char* text;
    MockTradingMode mode;
  };
  for (const auto item : {ModeCase{"fill", MockTradingMode::kFill},
                          ModeCase{"working", MockTradingMode::kWorking},
                          ModeCase{"reject", MockTradingMode::kReject}}) {
    ConfigFile file(Replace("\"fill\"", '"' + std::string(item.text) + '"'));
    ASSERT_TRUE(file.valid());
    TradingConfigLoader loader;
    ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
    EXPECT_EQ(loader.config().mode, item.mode);
    EXPECT_EQ(loader.config().report_interval_ns, 1000000U);
    EXPECT_EQ(loader.config().shutdown_timeout_ns, 5000000000ULL);
  }
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
  TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
  if (item.expected_path != nullptr) {
    EXPECT_STREQ(loader.error().field_path, item.expected_path);
  }
  EXPECT_EQ(loader.config().mode, MockTradingMode::kFill);
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
  TradingConfigLoader loader;
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
  TradingConfigLoader loader;
  std::string large = Replace("\"fill\"", "\"working\"");
  large.resize(base::kMaxConfigFileBytes, ' ');
  ConfigFile full(large);
  ASSERT_TRUE(full.valid());
  ASSERT_TRUE(loader.Load(full.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().mode, MockTradingMode::kWorking);
  large += ' ';
  ConfigFile excessive(large);
  ASSERT_TRUE(excessive.valid());
  EXPECT_FALSE(loader.Load(excessive.path()).ok());
  EXPECT_EQ(loader.config().mode, MockTradingMode::kFill);
  EXPECT_FALSE(loader.Load("/yquant-missing-trading-config").ok());
  ConfigFile good(kJson);
  ASSERT_TRUE(good.valid());
  ASSERT_TRUE(loader.Load(good.path()).ok());
  EXPECT_STREQ(loader.error().message, "");
}

TEST(TradingConfigTest, TypedValidationRejectsUnknownModesAndBadDurations) {
  base::ConfigError error;
  TradingConfig config;
  EXPECT_TRUE(ValidateTradingConfig(config, error).ok());
  config.mode = static_cast<MockTradingMode>(99);
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "mode");
  config = {};
  config.report_interval_ns = 0;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "report_interval_ms");
  config = {};
  config.shutdown_timeout_ns = UINT64_MAX;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "shutdown_timeout_ms");
}

constexpr std::string_view kPosition =
    R"({"venue":"SSE","symbol":"600000","total_qty":1000,"available_qty":800})";

// Modifies one position while retaining all newly required root fields.
std::string PositionJson(std::string_view before, std::string_view after) {
  std::string position(kPosition);
  const auto offset = position.find(before);
  if (offset != std::string::npos)
    position.replace(offset, before.size(), after);
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
  TradingConfigLoader loader;
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
  TradingConfigLoader loader;
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
  for (const std::size_t count : {64U, 65U}) {
    std::string positions = "[";
    for (std::size_t index = 0; index < count; ++index) {
      if (index != 0) positions += ',';
      char symbol[model::kSymbolLength + 1];
      std::snprintf(symbol, sizeof(symbol), "%0*zu",
                    static_cast<int>(model::kSymbolLength), index);
      std::string item(kPosition);
      item.replace(item.find("600000"), model::kSymbolLength, symbol);
      positions += item;
    }
    positions += ']';
    ConfigFile file(Replace("[]", positions));
    ASSERT_TRUE(file.valid());
    TradingConfigLoader loader;
    EXPECT_EQ(loader.Load(file.path()).ok(), count == 64)
        << loader.error().message;
    if (count == 64)
      EXPECT_EQ(loader.config().initial_account.position_count, 64U);
  }
  TradingConfigLoader loader;
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
  TradingConfig config;
  base::ConfigError error;
  config.initial_account.cash_balance = -1;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_cash");
  config.initial_account.cash_balance = INT64_MAX;
  config.initial_account.position_count = 65;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions");
  config.initial_account.position_count = 1;
  auto& position = config.initial_account.positions[0];
  position.instrument_id = {(1ULL << 32) | 600000};
  position.total_qty = INT64_MAX;
  position.available_qty = INT64_MAX;
  EXPECT_TRUE(ValidateTradingConfig(config, error).ok());
  position.reserved_qty = 1;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions[0].reserved_qty");
  position.reserved_qty = 0;
  position.instrument_id = {1};
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "initial_positions[0].symbol");
}

}  // namespace
}  // namespace yquant::strategyd

namespace yquant::strategyd {
namespace {
constexpr std::string_view kRemoteJson =
    R"({"adapter":"tcp","address":"127.0.0.1","port":19200,)"
    R"("client_id":7,"token":"strategy-7"})";

TEST(TradingConfigTest, LoadsRemoteEndpointAndDefaultTimeouts) {
  ConfigFile file(kRemoteJson);
  TradingConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().adapter, TradingAdapterKind::kTcp);
  EXPECT_STREQ(loader.config().remote.endpoint.address, "127.0.0.1");
  EXPECT_EQ(loader.config().remote.endpoint.port, 19200);
  EXPECT_EQ(loader.config().remote.client_id, 7U);
  EXPECT_STREQ(loader.config().remote.token, "strategy-7");
  EXPECT_EQ(loader.config().remote.connect_timeout_ns, 5000000000ULL);
  EXPECT_EQ(loader.config().remote.heartbeat_timeout_ns, 5000000000ULL);
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 5000000000ULL);
}

TEST(TradingConfigTest, RemoteSchemaIsIndependentOfKeyOrderAndMockAccount) {
  ConfigFile file(
      R"({"shutdown_timeout_ms":60000,"token":"abc","client_id":1,)"
      R"("port":65535,"connect_timeout_ms":1,"heartbeat_timeout_ms":60000,)"
      R"("address":"192.168.1.1","adapter":"tcp"})");
  TradingConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().remote.connect_timeout_ns, 1000000U);
  EXPECT_EQ(loader.config().remote.heartbeat_timeout_ns, 60000000000ULL);
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 60000000000ULL);
  for (const auto* field :
       {"mode", "initial_cash", "initial_positions", "report_interval_ms"}) {
    const std::string extra =
        std::string(kRemoteJson).substr(0, kRemoteJson.size() - 1) + ",\"" +
        field + "\":" +
        (std::string_view(field) == "mode"                ? "\"fill\""
         : std::string_view(field) == "initial_positions" ? "[]"
                                                          : "1") +
        '}';
    ConfigFile invalid(extra);
    EXPECT_FALSE(loader.Load(invalid.path()).ok()) << field;
    EXPECT_STREQ(loader.error().field_path, field);
  }
}

struct BadRemoteConfig {
  const char* name;
  const char* before;
  const char* after;
  const char* field;
};
class BadRemoteTradingConfigTest
    : public ::testing::TestWithParam<BadRemoteConfig> {};

TEST_P(BadRemoteTradingConfigTest, RejectsInvalidRemoteFields) {
  const auto item = GetParam();
  std::string json(kRemoteJson);
  json.replace(json.find(item.before), std::strlen(item.before), item.after);
  ConfigFile file(json);
  TradingConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_STREQ(loader.error().field_path, item.field);
  EXPECT_EQ(loader.config().adapter, TradingAdapterKind::kMock);
}

INSTANTIATE_TEST_SUITE_P(
    RemoteSchema, BadRemoteTradingConfigTest,
    ::testing::Values(
        BadRemoteConfig{"MissingAddress", "\"address\":\"127.0.0.1\",", "",
                        "address"},
        BadRemoteConfig{"MissingPort", "\"port\":19200,", "", "port"},
        BadRemoteConfig{"MissingClient", "\"client_id\":7,", "", "client_id"},
        BadRemoteConfig{"MissingToken", ",\"token\":\"strategy-7\"", "",
                        "token"},
        BadRemoteConfig{"WildcardAddress", "127.0.0.1", "0.0.0.0", "address"},
        BadRemoteConfig{"DnsAddress", "127.0.0.1", "localhost", "address"},
        BadRemoteConfig{"ZeroPort", "19200", "0", "port"},
        BadRemoteConfig{"BigPort", "19200", "65536", "port"},
        BadRemoteConfig{"ZeroClient", "\"client_id\":7", "\"client_id\":0",
                        "client_id"},
        BadRemoteConfig{"EmptyToken", "strategy-7", "", "token"},
        BadRemoteConfig{"TokenSpace", "strategy-7", "a b", "token"},
        BadRemoteConfig{"ZeroConnect",
                        "\"adapter\":", "\"connect_timeout_ms\":0,\"adapter\":",
                        "connect_timeout_ms"},
        BadRemoteConfig{"FractionalHeartbeat", "\"adapter\":",
                        "\"heartbeat_timeout_ms\":1.0,\"adapter\":",
                        "heartbeat_timeout_ms"}),
    [](const ::testing::TestParamInfo<BadRemoteConfig>& info) {
      return info.param.name;
    });

TEST(TradingConfigTest,
     TypedRemoteValidationChecksTerminatedFieldsAndTimeouts) {
  TradingConfig config;
  config.adapter = TradingAdapterKind::kTcp;
  config.remote.client_id = 1;
  std::memcpy(config.remote.token, "token", 6);
  base::ConfigError error;
  EXPECT_TRUE(ValidateTradingConfig(config, error).ok());
  config.remote.connect_timeout_ns = UINT64_MAX;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "connect_timeout_ms");
  config.remote.connect_timeout_ns = 5000000000ULL;
  config.remote.heartbeat_timeout_ns = 0;
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "heartbeat_timeout_ms");
  config.remote.heartbeat_timeout_ns = 5000000000ULL;
  std::memset(config.remote.token, 'x', sizeof(config.remote.token));
  EXPECT_FALSE(ValidateTradingConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "token");
}
}  // namespace
}  // namespace yquant::strategyd
