#include "yquant/base/format.h"

#include <unistd.h>

#include <array>
#include <cstdio>
#include <string>

#include "gtest/gtest.h"

namespace yquant::base {
namespace {

TEST(FormatTest, ReportsFullLengthAndTerminatesTruncatedBuffers) {
  std::array<char, 5> output{};
  EXPECT_EQ(FormatTo(output.data(), output.size(), "{}:{}", "order", 42), 8);
  EXPECT_STREQ(output.data(), "orde");
  EXPECT_EQ(FormatTo(output.data(), 1, "{}", 42), 2);
  EXPECT_EQ(output[0], '\0');
  EXPECT_EQ(FormatTo(nullptr, 0, "{}", 42), 2);
  EXPECT_EQ(FormatTo(output.data(), output.size(), "{:04x}", 42), 4);
  EXPECT_STREQ(output.data(), "002a");
}

TEST(FormatTest, PreservesCStringPrecisionAndEmbeddedTerminators) {
  const char text[] = "ab\0cd";
  EXPECT_EQ(CStringPrefix(text, 1), "a");
  EXPECT_EQ(CStringPrefix(text, 5), "ab");
  EXPECT_EQ(CStringPrefix(text, -1), "ab");
  EXPECT_TRUE(CStringPrefix(text, 0).empty());
  EXPECT_TRUE(CStringPrefix(nullptr, 0).empty());
}

TEST(FormatTest, PrintsMultipleBufferChunksAndReportsLength) {
  std::FILE* stream = std::tmpfile();
  ASSERT_NE(stream, nullptr);
  const std::string text(3000, 'x');
  EXPECT_EQ(Print(stream, "{}:{}", text, 42), 3003);
  ASSERT_EQ(std::fseek(stream, 0, SEEK_SET), 0);
  std::array<char, 3004> output{};
  EXPECT_EQ(std::fread(output.data(), 1, output.size(), stream), 3003);
  EXPECT_EQ(std::string(output.data(), 3003), text + ":42");
  EXPECT_EQ(std::fclose(stream), 0);
}

TEST(FormatTest, ReturnsFailureWhenTheStreamCannotBeWritten) {
  std::FILE* stream = std::fopen("/dev/null", "r");
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(Print(stream, "{}", 42), -1);
  std::fclose(stream);
}

}  // namespace
}  // namespace yquant::base
