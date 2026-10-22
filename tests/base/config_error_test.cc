#include "yquant/base/config_error.h"

#include <cstring>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace yquant::base {
namespace {

TEST(BaseConfigErrorTest, OwnsTextAndClearsPreviousDiagnostics) {
  ConfigError error;
  EXPECT_STREQ(error.field_path, "");
  EXPECT_STREQ(error.message, "");
  EXPECT_FALSE(error.truncated);
  std::string path = "connection.address";
  std::string reason = "must not be empty";
  error.Set(path, reason);
  path.assign(path.size(), 'x');
  reason.assign(reason.size(), 'x');
  EXPECT_STREQ(error.field_path, "connection.address");
  EXPECT_STREQ(error.message, "must not be empty");
  EXPECT_FALSE(error.truncated);
  error.Clear();
  EXPECT_STREQ(error.field_path, "");
  EXPECT_STREQ(error.message, "");
  EXPECT_FALSE(error.truncated);
}

TEST(BaseConfigErrorTest, TruncatesAndTerminatesEachBufferIndependently) {
  ConfigError error;
  error.Set(std::string(256, 'p'), "reason");
  EXPECT_TRUE(error.truncated);
  EXPECT_EQ(std::strlen(error.field_path), 255U);
  EXPECT_STREQ(error.message, "reason");
  error.Set("field", std::string(512, 'm'));
  EXPECT_TRUE(error.truncated);
  EXPECT_STREQ(error.field_path, "field");
  EXPECT_EQ(std::strlen(error.message), 511U);
  error.Set(std::string(255, 'p'), std::string(511, 'm'));
  EXPECT_FALSE(error.truncated);
  error.Clear();
  EXPECT_FALSE(error.truncated);
}

TEST(BaseConfigErrorTest, SupportsViewsIntoItsExistingStorage) {
  ConfigError error;
  error.Set("root.field", "invalid value");
  error.Set(std::string_view(error.field_path).substr(5),
            std::string_view(error.message).substr(8));
  EXPECT_STREQ(error.field_path, "field");
  EXPECT_STREQ(error.message, "value");
  EXPECT_FALSE(error.truncated);
}

TEST(BaseConfigErrorTest, InvalidConfigCopiesDiagnosticAndReturnsStatus) {
  ConfigError error;
  const Status result = InvalidConfig("size", "must be positive", error);
  EXPECT_EQ(result.code, ErrorCode::kInvalidArgument);
  EXPECT_FALSE(result.system_error);
  EXPECT_STREQ(error.field_path, "size");
  EXPECT_STREQ(error.message, "must be positive");
}

}  // namespace
}  // namespace yquant::base
