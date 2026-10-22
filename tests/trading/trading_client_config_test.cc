#include "yquant/trading/trading_client_config.h"

#include <cstring>
#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "test_config_file.h"

namespace yquant::trading {
namespace {
using ::yquant::testing::ConfigFile;
constexpr std::string_view kClientJson =
    R"({"adapter":"tcp","address":"127.0.0.1","port":19200,)"
    R"("client_id":7,"token":"strategy-7"})";

TEST(TradingClientConfigTest, LoadsRemoteEndpointAndDefaultTimeouts) {
  ConfigFile file(kClientJson);
  trading::TradingClientConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_STREQ(loader.config().endpoint.address, "127.0.0.1");
  EXPECT_EQ(loader.config().endpoint.port, 19200);
  EXPECT_EQ(loader.config().client_id, 7U);
  EXPECT_STREQ(loader.config().token, "strategy-7");
  EXPECT_EQ(loader.config().connect_timeout_ns, 5000000000ULL);
  EXPECT_EQ(loader.config().heartbeat_timeout_ns, 5000000000ULL);
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 5000000000ULL);
}

TEST(TradingClientConfigTest,
     RemoteSchemaIsIndependentOfKeyOrderAndMockAccount) {
  ConfigFile file(
      R"({"shutdown_timeout_ms":60000,"token":"abc","client_id":1,)"
      R"("port":65535,"connect_timeout_ms":1,"heartbeat_timeout_ms":60000,)"
      R"("address":"192.168.1.1","adapter":"tcp"})");
  trading::TradingClientConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.config().connect_timeout_ns, 1000000U);
  EXPECT_EQ(loader.config().heartbeat_timeout_ns, 60000000000ULL);
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 60000000000ULL);
  for (const auto* field :
       {"mode", "initial_cash", "initial_positions", "report_interval_ms"}) {
    const std::string extra =
        std::string(kClientJson).substr(0, kClientJson.size() - 1) + ",\"" +
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

struct BadClientConfig {
  const char* name;
  const char* before;
  const char* after;
  const char* field;
};
class BadTradingClientConfigTest
    : public ::testing::TestWithParam<BadClientConfig> {};

TEST_P(BadTradingClientConfigTest, RejectsInvalidRemoteFields) {
  const auto item = GetParam();
  std::string json(kClientJson);
  json.replace(json.find(item.before), std::strlen(item.before), item.after);
  ConfigFile file(json);
  trading::TradingClientConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_STREQ(loader.error().field_path, item.field);
}

INSTANTIATE_TEST_SUITE_P(
    RemoteSchema, BadTradingClientConfigTest,
    ::testing::Values(
        BadClientConfig{"InProcessAdapter", "tcp", "mock", "adapter"},
        BadClientConfig{"MissingAddress", "\"address\":\"127.0.0.1\",", "",
                        "address"},
        BadClientConfig{"MissingPort", "\"port\":19200,", "", "port"},
        BadClientConfig{"MissingClient", "\"client_id\":7,", "", "client_id"},
        BadClientConfig{"MissingToken", ",\"token\":\"strategy-7\"", "",
                        "token"},
        BadClientConfig{"WildcardAddress", "127.0.0.1", "0.0.0.0", "address"},
        BadClientConfig{"DnsAddress", "127.0.0.1", "localhost", "address"},
        BadClientConfig{"ZeroPort", "19200", "0", "port"},
        BadClientConfig{"BigPort", "19200", "65536", "port"},
        BadClientConfig{"ZeroClient", "\"client_id\":7", "\"client_id\":0",
                        "client_id"},
        BadClientConfig{"EmptyToken", "strategy-7", "", "token"},
        BadClientConfig{"TokenSpace", "strategy-7", "a b", "token"},
        BadClientConfig{"ZeroConnect",
                        "\"adapter\":", "\"connect_timeout_ms\":0,\"adapter\":",
                        "connect_timeout_ms"},
        BadClientConfig{"FractionalHeartbeat", "\"adapter\":",
                        "\"heartbeat_timeout_ms\":1.0,\"adapter\":",
                        "heartbeat_timeout_ms"}),
    [](const ::testing::TestParamInfo<BadClientConfig>& info) {
      return info.param.name;
    });

TEST(TradingClientConfigTest,
     TypedRemoteValidationChecksTerminatedFieldsAndTimeouts) {
  trading::TradingClientConfig config;
  config.endpoint.port = 19200;
  config.client_id = 1;
  std::memcpy(config.token, "token", 6);
  base::ConfigError error;
  EXPECT_TRUE(trading::ValidateTradingClientConfig(config, error).ok());
  config.connect_timeout_ns = UINT64_MAX;
  EXPECT_FALSE(trading::ValidateTradingClientConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "connect_timeout_ms");
  config.connect_timeout_ns = 5000000000ULL;
  config.heartbeat_timeout_ns = 0;
  EXPECT_FALSE(trading::ValidateTradingClientConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "heartbeat_timeout_ms");
  config.heartbeat_timeout_ns = 5000000000ULL;
  std::memset(config.token, 'x', sizeof(config.token));
  EXPECT_FALSE(trading::ValidateTradingClientConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "token");
}
}  // namespace
}  // namespace yquant::trading
