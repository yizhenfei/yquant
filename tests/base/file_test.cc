#include "yquant/base/file.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/base/path.h"

namespace yquant::base {
namespace {

class FileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/yquant-file-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern), nullptr);
    directory_ = pattern;
  }
  void TearDown() override { EXPECT_TRUE(RemoveTree(directory_).ok()); }
  std::string Path(std::string_view name) const {
    return JoinPath(directory_, name);
  }
  void Write(std::string_view name, std::string_view contents) {
    auto* file = std::fopen(Path(name).c_str(), "wb");
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(std::fwrite(contents.data(), 1, contents.size(), file),
              contents.size());
    ASSERT_EQ(std::fclose(file), 0);
  }
  std::string directory_;
};

TEST(FilePathTest, KeepsRootsAndNormalizesComponents) {
  EXPECT_EQ(JoinPath("/a", "/b"), "/b");
  EXPECT_EQ(JoinPath("/a/", "b"), "/a/b");
  EXPECT_EQ(PathFilename("/a/b"), "b");
  EXPECT_TRUE(PathFilename("/a/b/").empty());
  EXPECT_EQ(ParentPath("/a/b"), "/a");
  EXPECT_EQ(ParentPath("/a"), "/");
  EXPECT_EQ(ParentPath("/"), "/");
  EXPECT_EQ(ParentPath("a"), "");
  EXPECT_EQ(ParentPath("/a//b"), "/a");
  EXPECT_EQ(NormalizePath("/a//./b/../c/"), "/a/c");
  EXPECT_EQ(NormalizePath("/../../a"), "/a");
  EXPECT_EQ(NormalizePath("../../a/../b"), "../../b");
  EXPECT_EQ(NormalizePath("a/.."), ".");
  EXPECT_EQ(NormalizePath(""), "");
}

TEST_F(FileTest, CanonicalizesExistingPrefixAndPreservesMissingSuffix) {
  bool created = false;
  ASSERT_TRUE(CreateDirectories(Path("target"), created).ok());
  ASSERT_TRUE(created);
  ASSERT_EQ(::symlink(Path("target").c_str(), Path("link").c_str()), 0);
  std::string expected;
  ASSERT_TRUE(CanonicalizePath(Path("target"), expected).ok());
  std::string actual;
  ASSERT_TRUE(CanonicalizePath(Path("link/missing/../file"), actual).ok());
  EXPECT_EQ(actual, JoinPath(expected, "file"));
  ASSERT_TRUE(CreateDirectories(Path("target"), created).ok());
  EXPECT_FALSE(created);
}

TEST_F(FileTest, EnumeratesRecursivelyWithoutFollowingDirectorySymlinks) {
  bool created = false;
  ASSERT_TRUE(CreateDirectories(Path("sub/nested"), created).ok());
  Write("sub/data", "data");
  ASSERT_EQ(::symlink(directory_.c_str(), Path("sub/loop").c_str()), 0);
  std::vector<std::string> entries;
  ASSERT_TRUE(
      ListDirectory(directory_, entries, DirectoryTraversal::kRecursive).ok());
  std::sort(entries.begin(), entries.end());
  EXPECT_EQ(entries,
            (std::vector<std::string>{Path("sub"), Path("sub/data"),
                                      Path("sub/loop"), Path("sub/nested")}));
  const auto previous = entries;
  EXPECT_FALSE(ListDirectory(Path("absent"), entries).ok());
  EXPECT_EQ(entries, previous);
  ASSERT_TRUE(ListDirectory(Path("sub/loop"), entries).ok());
  EXPECT_EQ(entries, (std::vector<std::string>{Path("sub/loop/sub")}));
}

TEST_F(FileTest, DirectoryReaderPreservesNameAtEofAndSupportsReopen) {
  DirectoryReader reader;
  std::string_view name = "unchanged";
  bool done = false;
  EXPECT_EQ(reader.Next(name, done).code, ErrorCode::kInvalidState);
  ASSERT_TRUE(reader.Open(directory_).ok());
  EXPECT_EQ(reader.Open(directory_).code, ErrorCode::kInvalidState);
  ASSERT_TRUE(reader.Next(name, done).ok());
  EXPECT_TRUE(done);
  EXPECT_EQ(name, "unchanged");
  reader.Close();
  Write("entry", "x");
  ASSERT_TRUE(reader.Open(directory_).ok());
  ASSERT_TRUE(reader.Next(name, done).ok());
  EXPECT_FALSE(done);
  EXPECT_EQ(name, "entry");
  ASSERT_TRUE(reader.Next(name, done).ok());
  EXPECT_TRUE(done);
}

TEST_F(FileTest, CopyRejectsSameFileAndSymlinksAndTruncatesOnlyWithOverwrite) {
  Write("input", "abc");
  Write("output", "longer original");
  EXPECT_FALSE(CopyFile(Path("input"), Path("output"), false).ok());
  uint64_t size = 0;
  ASSERT_TRUE(FileSize(Path("output"), size).ok());
  EXPECT_EQ(size, 15);
  ASSERT_TRUE(CopyFile(Path("input"), Path("output"), true).ok());
  ASSERT_TRUE(FileSize(Path("output"), size).ok());
  EXPECT_EQ(size, 3);
  EXPECT_FALSE(CopyFile(Path("input"), Path("input"), true).ok());
  ASSERT_TRUE(FileSize(Path("input"), size).ok());
  EXPECT_EQ(size, 3);
  ASSERT_EQ(::symlink(Path("input").c_str(), Path("alias").c_str()), 0);
  EXPECT_FALSE(CopyFile(Path("input"), Path("alias"), true).ok());
  EXPECT_FALSE(CopyFile(Path("alias"), Path("new"), false).ok());
  EXPECT_FALSE(FileExists(Path("new")));
  size = 99;
  EXPECT_FALSE(FileSize(Path("absent"), size).ok());
  EXPECT_EQ(size, 99);
}

TEST_F(FileTest, RemoveTreeDoesNotFollowSymlinksOrDeleteTheirTargets) {
  bool created = false;
  ASSERT_TRUE(CreateDirectories(Path("tree/child"), created).ok());
  Write("keep", "safe");
  ASSERT_EQ(::symlink(Path("keep").c_str(), Path("tree/file-link").c_str()), 0);
  ASSERT_EQ(::symlink(directory_.c_str(), Path("tree/directory-link").c_str()),
            0);
  ASSERT_TRUE(RemoveTree(Path("tree")).ok());
  EXPECT_TRUE(FileExists(Path("keep")));
  EXPECT_FALSE(FileExists(Path("tree")));
  EXPECT_TRUE(RemoveTree(Path("tree")).ok());
  bool removed = true;
  EXPECT_TRUE(RemoveFile(Path("absent"), removed).ok());
  EXPECT_FALSE(removed);
  EXPECT_EQ(RemoveTree("/").code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(RemoveTree(Path(".")).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(RemoveTree(Path("..")).code, ErrorCode::kInvalidArgument);
}

TEST_F(FileTest, InvalidPathsPreserveOutputs) {
  const std::string invalid("path\0suffix", 11);
  bool created = false;
  EXPECT_EQ(CreateDirectories(invalid, created).code,
            ErrorCode::kInvalidArgument);
  std::string output = "unchanged";
  EXPECT_EQ(CanonicalizePath(invalid, output).code,
            ErrorCode::kInvalidArgument);
  EXPECT_EQ(output, "unchanged");
  std::vector<std::string> entries{"unchanged"};
  EXPECT_EQ(ListDirectory(invalid, entries).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(entries, (std::vector<std::string>{"unchanged"}));
}

}  // namespace
}  // namespace yquant::base
