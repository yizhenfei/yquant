#include "yquant/base/path.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "gtest/gtest.h"
#include "scoped_working_directory.h"

namespace yquant::base {
namespace {
TEST(PathTest, ResolvesRelativePathsFromWorkingDirectoryWithoutOpeningFiles) {
  char directory[] = "/tmp/yquant-path-test-XXXXXX";
  ASSERT_NE(::mkdtemp(directory), nullptr);
  {
    ::yquant::testing::ScopedWorkingDirectory cwd(directory);
    ASSERT_TRUE(cwd.valid());
    std::array<char, 4096> output{};
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(ResolvePath("missing/subdir/file.json", output).ok());
    EXPECT_EQ(output.data(), (current / "missing/subdir/file.json").string());
    ASSERT_TRUE(ResolvePath("../outside.json", output).ok());
    EXPECT_EQ(output.data(), (current / "../outside.json").string());
  }
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

TEST(PathTest, PreservesAbsoluteSpellingAndSupportsAliasedOutput) {
  std::array<char, 4096> output{};
  ASSERT_TRUE(ResolvePath("/missing/../file.json", output).ok());
  EXPECT_STREQ(output.data(), "/missing/../file.json");
  std::strcpy(output.data(), "catalog.json");
  std::error_code error;
  const auto expected = std::filesystem::current_path(error) / "catalog.json";
  ASSERT_FALSE(error);
  ASSERT_TRUE(ResolvePath(output.data(), output).ok());
  EXPECT_EQ(output.data(), expected.string());
}

TEST(PathTest, InvalidInputAndInsufficientCapacityPreserveOutput) {
  std::array<char, 32> output;
  output.fill('x');
  const auto before = output;
  const char embedded_nul[] = "file\0.json";
  EXPECT_EQ(ResolvePath("", output).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(ResolvePath({embedded_nul, sizeof(embedded_nul) - 1}, output).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(ResolvePath("/" + std::string(40, 'a'), output).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(output, before);
  EXPECT_EQ(ResolvePath("/a", {}).code, ErrorCode::kInvalidArgument);
  std::array<char, 3> exact{};
  ASSERT_TRUE(ResolvePath("/a", exact).ok());
  EXPECT_STREQ(exact.data(), "/a");
}
TEST(PathTest, Fits4095BytesAndPreservesOutputOnOverflow) {
  std::array<char, 4096> output{};
  std::string path(4095, 'x');
  path.front() = '/';
  ASSERT_TRUE(ResolvePath(path, output).ok());
  EXPECT_EQ(std::string_view(output.data()), path);
  EXPECT_EQ(output.back(), '\0');
  const auto unchanged = output;
  path += 'x';
  EXPECT_EQ(ResolvePath(path, output).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(output, unchanged);
}

}  // namespace
}  // namespace yquant::base
