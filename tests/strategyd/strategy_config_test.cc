#include "yquant/strategy/strategy_config.h"

#include <cstring>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace yquant::strategy {
namespace {

using base::ErrorCode;

StrategyId MakeId(std::string_view text = "test-strategy") {
  StrategyId id;
  EXPECT_EQ(id.Assign(text), ErrorCode::kOk);
  return id;
}

class CapturingConfig final : public StrategyConfig {
 public:
  ErrorCode result = ErrorCode::kOk;
  int calls = 0;
  std::string id_during_load;
  std::string parameters;

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError& error) noexcept override {
    ++calls;
    id_during_load = strategy_id();
    parameters = json;
    if (result != ErrorCode::kOk) {
      error.Set("quantity", "quantity must be positive");
    }
    return result;
  }
};

TEST(StrategyIdTest, RetainsAll64BytesAndDoesNotTruncate) {
  StrategyId id;
  EXPECT_TRUE(id.view().empty());
  const std::string longest(kMaxStrategyIdBytes, 'x');
  ASSERT_EQ(id.Assign(longest), ErrorCode::kOk);
  EXPECT_EQ(id.view(), longest);
  EXPECT_EQ(id.Assign(std::string(kMaxStrategyIdBytes + 1, 'y')),
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(id.view(), longest);
}

TEST(StrategyIdTest, ValidatesDecodedUtf8AndEmbeddedNul) {
  StrategyId id;
  for (const std::string& invalid :
       {std::string(), std::string("a\0b", 3), std::string("\x80", 1),
        std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4), std::string("\xe4\xb8", 2)}) {
    SCOPED_TRACE(::testing::PrintToString(invalid));
    EXPECT_EQ(id.Assign(invalid), ErrorCode::kInvalidArgument);
  }
  ASSERT_EQ(id.Assign("策略-α"), ErrorCode::kOk);
  EXPECT_EQ(id.view(), "策略-α");
}

TEST(StrategyIdTest, CopiesAndOwnsInputWithoutNormalization) {
  std::string input = " Case Sensitive ";
  StrategyId first;
  ASSERT_EQ(first.Assign(input), ErrorCode::kOk);
  StrategyId second = first;
  input.assign(input.size(), 'x');
  ASSERT_EQ(first.Assign("changed"), ErrorCode::kOk);
  EXPECT_EQ(second.view(), " Case Sensitive ");
}

TEST(StrategyConfigTest, InitializesCommonIdBeforeParametersAndOnlyOnce) {
  CapturingConfig config;
  ConfigError error;
  error.Set("stale", "stale error");
  EXPECT_FALSE(config.ready());
  EXPECT_TRUE(config.strategy_id().empty());
  std::string json = R"({"label":"owned"})";
  ASSERT_EQ(config.Initialize(MakeId(), json, error), ErrorCode::kOk);
  EXPECT_TRUE(config.ready());
  EXPECT_EQ(config.id_during_load, "test-strategy");
  EXPECT_EQ(config.strategy_id(), "test-strategy");
  EXPECT_STREQ(error.field_path, "");
  EXPECT_STREQ(error.message, "");
  json.assign(json.size(), 'x');
  EXPECT_EQ(config.parameters, R"({"label":"owned"})");
  EXPECT_EQ(config.Initialize(MakeId("other"), "{}", error),
            ErrorCode::kInvalidState);
  EXPECT_EQ(config.calls, 1);
  EXPECT_EQ(config.strategy_id(), "test-strategy");
  EXPECT_TRUE(config.ready());
}

TEST(StrategyConfigTest, FailedInitializationCannotBeRetried) {
  CapturingConfig config;
  config.result = ErrorCode::kInvalidArgument;
  ConfigError error;
  EXPECT_EQ(config.Initialize(MakeId(), "{}", error),
            ErrorCode::kInvalidArgument);
  EXPECT_FALSE(config.ready());
  EXPECT_STREQ(error.field_path, "quantity");
  EXPECT_STREQ(error.message, "quantity must be positive");
  config.result = ErrorCode::kOk;
  EXPECT_EQ(config.Initialize(MakeId(), "{}", error), ErrorCode::kInvalidState);
  EXPECT_FALSE(config.ready());
  EXPECT_EQ(config.calls, 1);
  EXPECT_STREQ(error.field_path, "quantity");
  EXPECT_STREQ(error.message, "quantity must be positive");
}

TEST(StrategyConfigTest, EmptyIdFailsBeforeCallingTheParameterLoader) {
  CapturingConfig config;
  ConfigError error;
  EXPECT_EQ(config.Initialize(StrategyId{}, "{}", error),
            ErrorCode::kInvalidArgument);
  EXPECT_FALSE(config.ready());
  EXPECT_EQ(config.calls, 0);
  EXPECT_STREQ(error.field_path, "strategy_id");
  EXPECT_EQ(config.Initialize(MakeId(), "{}", error), ErrorCode::kInvalidState);
}

TEST(StrategyConfigTest, DefaultParametersAcceptOnlyEmptyObjects) {
  for (std::string_view json : {"{}", " { \n\t } "}) {
    StrategyConfig config;
    ConfigError error;
    EXPECT_EQ(config.Initialize(MakeId(), json, error), ErrorCode::kOk);
    EXPECT_TRUE(config.ready());
  }
  for (std::string_view json : {R"({"unexpected":1})", "[]", "null", ""}) {
    StrategyConfig config;
    ConfigError error;
    EXPECT_EQ(config.Initialize(MakeId(), json, error),
              ErrorCode::kInvalidArgument);
    EXPECT_FALSE(config.ready());
    EXPECT_NE(std::strlen(error.message), 0U);
  }
}

TEST(ConfigErrorTest, OwnsTextAndClearsPreviousDiagnostics) {
  ConfigError error;
  std::string path = "quote_qty";
  std::string reason = "must be positive";
  error.Set(path, reason);
  path.assign(path.size(), 'x');
  reason.assign(reason.size(), 'x');
  EXPECT_STREQ(error.field_path, "quote_qty");
  EXPECT_STREQ(error.message, "must be positive");
  EXPECT_FALSE(error.truncated);
  error.Clear();
  EXPECT_STREQ(error.field_path, "");
  EXPECT_STREQ(error.message, "");
  EXPECT_FALSE(error.truncated);
}

TEST(ConfigErrorTest, TruncatesBothBuffersAndAlwaysTerminates) {
  ConfigError error;
  const std::string path(sizeof(error.field_path) * 2, 'p');
  const std::string reason(sizeof(error.message) * 2, 'm');
  error.Set(path, reason);
  EXPECT_TRUE(error.truncated);
  EXPECT_EQ(std::strlen(error.field_path), sizeof(error.field_path) - 1);
  EXPECT_EQ(std::strlen(error.message), sizeof(error.message) - 1);
  error.Set("short", "message");
  EXPECT_FALSE(error.truncated);
  EXPECT_STREQ(error.field_path, "short");
  EXPECT_STREQ(error.message, "message");
}

}  // namespace
}  // namespace yquant::strategy
