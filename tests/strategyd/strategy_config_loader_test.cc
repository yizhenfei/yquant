#include "strategyd/strategy_config_loader.h"

#include <stddef.h>

#include <string>
#include <string_view>

#include "gtest/gtest.h"
#include "test_config_file.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {
namespace {

using ::yquant::testing::ConfigFile;

struct InvalidConfig {
  const char* name;
  const char* json;
};

class InvalidConfigTest : public ::testing::TestWithParam<InvalidConfig> {};

TEST_P(InvalidConfigTest, RejectsInvalidJsonAndPublicFields) {
  ConfigFile file(GetParam().json);
  ASSERT_TRUE(file.valid());
  StrategyConfigLoader loader;
  EXPECT_FALSE(loader.Load(file.path()).ok());
  EXPECT_NE(loader.error().message[0], '\0');
}

INSTANTIATE_TEST_SUITE_P(
    JsonDocuments, InvalidConfigTest,
    ::testing::Values(
        InvalidConfig{"Empty", ""},
        InvalidConfig{"Malformed", R"({"strategy_id":"x","parameters":{)"},
        InvalidConfig{"TrailingDocument",
                      R"({"strategy_id":"x","parameters":{}} {})"},
        InvalidConfig{"TrailingGarbage",
                      R"({"strategy_id":"x","parameters":{}} garbage)"},
        InvalidConfig{"Comment",
                      R"({"strategy_id":"x",/*comment*/"parameters":{}})"},
        InvalidConfig{"TrailingComma",
                      R"({"strategy_id":"x","parameters":{},})"},
        InvalidConfig{"NumberLeadingZero",
                      R"({"strategy_id":"x","parameters":{"value":01}})"},
        InvalidConfig{"NumberMissingFraction",
                      R"({"strategy_id":"x","parameters":{"value":1.}})"},
        InvalidConfig{"NumberMissingExponent",
                      R"({"strategy_id":"x","parameters":{"value":1e+}})"},
        InvalidConfig{"NumberInvalidSuffix",
                      R"({"strategy_id":"x","parameters":{"value":1x}})"},
        InvalidConfig{"InvalidBoolean",
                      R"({"strategy_id":"x","parameters":{"value":tru}})"},
        InvalidConfig{"InvalidNull",
                      R"({"strategy_id":"x","parameters":{"value":nul}})"},
        InvalidConfig{"RootArray", "[]"}, InvalidConfig{"RootNull", "null"},
        InvalidConfig{"MissingId", R"({"parameters":{}})"},
        InvalidConfig{"MissingParameters", R"({"strategy_id":"x"})"},
        InvalidConfig{"UnknownRoot",
                      R"({"strategy_id":"x","parameters":{},"other":1})"},
        InvalidConfig{"IdInteger", R"({"strategy_id":1,"parameters":{}})"},
        InvalidConfig{"IdNull", R"({"strategy_id":null,"parameters":{}})"},
        InvalidConfig{"IdEmpty", R"({"strategy_id":"","parameters":{}})"},
        InvalidConfig{"IdEmbeddedNul",
                      R"({"strategy_id":"a\u0000b","parameters":{}})"},
        InvalidConfig{"IdUnpairedSurrogate",
                      R"({"strategy_id":"\ud800","parameters":{}})"},
        InvalidConfig{"ParameterValueUnpairedSurrogate",
                      R"({"strategy_id":"x","parameters":)"
                      R"({"nested":{"label":"\udfff"}}})"},
        InvalidConfig{"ParameterKeyUnpairedSurrogate",
                      R"({"strategy_id":"x","parameters":)"
                      R"({"nested":{"\ud800":1}}})"},
        InvalidConfig{"ParametersArray",
                      R"({"strategy_id":"x","parameters":[]})"},
        InvalidConfig{"ParametersNull",
                      R"({"strategy_id":"x","parameters":null})"},
        InvalidConfig{"ParametersString",
                      R"({"strategy_id":"x","parameters":"{}"})"},
        InvalidConfig{"DuplicateRoot",
                      R"({"strategy_id":"x","strategy_id":"y",)"
                      R"("parameters":{}})"},
        InvalidConfig{
            "DuplicateDecodedRoot",
            R"({"strategy_id":"x","\u0073trategy_id":"y","parameters":{}})"},
        InvalidConfig{"DuplicateParameter",
                      R"({"strategy_id":"x","parameters":{"a":1,"a":2}})"},
        InvalidConfig{"DuplicateDecodedParameter",
                      R"({"strategy_id":"x","parameters":{"a":1,"\u0061":2}})"},
        InvalidConfig{
            "DuplicateNestedObject",
            R"({"strategy_id":"x","parameters":{"nested":{"a":1,"a":2}}})"},
        InvalidConfig{"DuplicateInsideArray",
                      R"({"strategy_id":"x","parameters":)"
                      R"({"items":[{"a":1,"\u0061":2}]}})"},
        InvalidConfig{"DuplicateEmptyKey", R"({"strategy_id":"x","parameters":)"
                                           R"({"items":[{"":1,"":2}]}})"},
        InvalidConfig{"DuplicateDecodedNulKey",
                      R"({"strategy_id":"x","parameters":{"nested":)"
                      R"({"a\u0000b":1,"a\u0000\u0062":2}}})"}),
    [](const ::testing::TestParamInfo<InvalidConfig>& info) {
      return info.param.name;
    });

TEST(StrategyConfigLoaderTest, DecodesIdAndKeepsOriginalParameterSource) {
  const std::string parameters =
      "{ \"large_value\" : 9007199254740993, \"text\":\"\\u0061\", "
      "\"floating\":1.25e+2 }";
  ConfigFile file(
      "{\"strategy_id\":\"\\u7b56\\u7565-001\","
      "\"parameters\":" +
      parameters + "}");
  ASSERT_TRUE(file.valid());
  StrategyConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.strategy_id().view(), "策略-001");
  EXPECT_EQ(loader.parameters_json(), parameters);
}

TEST(StrategyConfigLoaderTest, IdLimitCountsDecodedUtf8Bytes) {
  std::string escaped;
  for (int i = 0; i < 21; ++i) {
    escaped += R"(\u7b56)";
  }
  ConfigFile longest("{\"strategy_id\":\"" + escaped +
                     "a\",\"parameters\":{}}");
  ASSERT_TRUE(longest.valid());
  StrategyConfigLoader loader;
  ASSERT_TRUE(loader.Load(longest.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.strategy_id().view().size(), 64U);
  ConfigFile too_long("{\"strategy_id\":\"" + escaped +
                      "ab\",\"parameters\":{}}");
  ASSERT_TRUE(too_long.valid());
  EXPECT_FALSE(loader.Load(too_long.path()).ok());
  EXPECT_STREQ(loader.error().field_path, "strategy_id");
}

TEST(StrategyConfigLoaderTest, IdLimitDoesNotCountJsonEscapeText) {
  std::string id;
  for (size_t i = 0; i < strategy::kMaxStrategyIdBytes; ++i) {
    id += R"(\u0061)";
  }
  ConfigFile file("{\"strategy_id\":\"" + id + "\",\"parameters\":{}}");
  ASSERT_TRUE(file.valid());
  StrategyConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.strategy_id().view(),
            std::string(strategy::kMaxStrategyIdBytes, 'a'));
}

TEST(StrategyConfigLoaderTest, KeepsNumbersOutsideHostNumericRanges) {
  const std::string parameters =
      R"({"integer":18446744073709551616000000000000,)"
      R"("negative":-18446744073709551616000000000000,)"
      R"("exponent":1e9999,"nested":[-0,0.1,1e-9999,true,false,null]})";
  ConfigFile file(R"({"strategy_id":"numeric","parameters":)" + parameters +
                  '}');
  ASSERT_TRUE(file.valid());
  StrategyConfigLoader loader;
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.parameters_json(), parameters);
}

TEST(StrategyConfigLoaderTest, RejectsInvalidUtf8AnywhereInTheDocument) {
  const char* prefixes[] = {
      "{\"strategy_id\":\"",
      "{\"strategy_id\":\"x\",\"parameters\":{\"label\":\""};
  for (const char* prefix : prefixes) {
    std::string json = prefix;
    json += '\x80';
    json += "\",\"parameters\":{}}";
    if (std::string_view(prefix).find("label") != std::string_view::npos) {
      json = std::string(prefix) + '\x80' + "\"}}";
    }
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    StrategyConfigLoader loader;
    EXPECT_FALSE(loader.Load(file.path()).ok());
  }
}

TEST(StrategyConfigLoaderTest, EqualKeysInDifferentObjectsAreAllowed) {
  ConfigFile file(
      R"({"strategy_id":"x","parameters":)"
      R"({"one":{"a":1},"two":{"a":2},"items":[{"a":3},{"a":4}]}})");
  ASSERT_TRUE(file.valid());
  StrategyConfigLoader loader;
  EXPECT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
}

TEST(StrategyConfigLoaderTest, LimitsFileSizeAndAllowsTheExactLimit) {
  std::string json = R"({"strategy_id":"x","parameters":{}})";
  json.resize(base::kMaxConfigFileBytes, ' ');
  ConfigFile largest(json);
  ASSERT_TRUE(largest.valid());
  StrategyConfigLoader loader;
  EXPECT_TRUE(loader.Load(largest.path()).ok()) << loader.error().message;
  json += ' ';
  ConfigFile excessive(json);
  ASSERT_TRUE(excessive.valid());
  EXPECT_FALSE(loader.Load(excessive.path()).ok());
}

TEST(StrategyConfigLoaderTest, AcceptsDepth64AndRejectsDepth65) {
  for (const size_t depth :
       {base::kMaxConfigDepth, base::kMaxConfigDepth + 1}) {
    SCOPED_TRACE(depth);
    std::string json = R"({"strategy_id":"x","parameters":{"nested":)";
    // Root and parameters account for two container levels.
    const size_t arrays = depth - 2;
    json.append(arrays, '[');
    json += '0';
    json.append(arrays, ']');
    json += "}}";
    ConfigFile file(json);
    ASSERT_TRUE(file.valid());
    StrategyConfigLoader loader;
    EXPECT_EQ(loader.Load(file.path()).ok(), depth == base::kMaxConfigDepth)
        << loader.error().message;
  }
}

TEST(StrategyConfigLoaderTest, ReportsReadErrorsAndCanBeReused) {
  StrategyConfigLoader loader;
  for (const char* path : {static_cast<const char*>(nullptr), "",
                           "/yquant-no-such-config.json", "/tmp"}) {
    EXPECT_FALSE(loader.Load(path).ok());
    EXPECT_NE(loader.error().message[0], '\0');
  }
  ConfigFile file(R"({"strategy_id":"reused","parameters":{}})");
  ASSERT_TRUE(file.valid());
  ASSERT_TRUE(loader.Load(file.path()).ok()) << loader.error().message;
  EXPECT_EQ(loader.strategy_id().view(), "reused");
  EXPECT_STREQ(loader.error().message, "");
  loader.Reset();
  EXPECT_TRUE(loader.strategy_id().view().empty());
  EXPECT_TRUE(loader.parameters_json().empty());
}

}  // namespace
}  // namespace yquant::strategyd
