#ifndef YQUANT_TESTS_FILE_TEST_SUPPORT_H_
#define YQUANT_TESTS_FILE_TEST_SUPPORT_H_

// Filesystem conveniences for tests. Unchecked overloads report failures
// through GoogleTest, so setup and cleanup cannot silently ignore POSIX I/O
// failures.

#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "yquant/base/file.h"
#include "yquant/base/path.h"

namespace yquant::testing::files {

// Carries a test path spelling, without opening or resolving it on
// construction.
class FilePath {
 public:
  FilePath() = default;
  explicit FilePath(const char* text) : text_(text) {}
  explicit FilePath(std::string_view text) : text_(text) {}
  explicit FilePath(std::string text) : text_(std::move(text)) {}
  FilePath(const FilePath&) = default;
  FilePath& operator=(const FilePath&) = default;
  const std::string& string() const { return text_; }
  const std::string& native() const { return text_; }
  const char* c_str() const { return text_.c_str(); }
  bool is_relative() const { return !text_.starts_with('/'); }
  FilePath Parent() const { return FilePath(base::ParentPath(text_)); }
  FilePath Filename() const { return FilePath(base::PathFilename(text_)); }
  FilePath Extension() const {
    const auto name = base::PathFilename(text_);
    const auto dot = name.find_last_of('.');
    return FilePath(dot == std::string_view::npos || dot == 0
                        ? std::string{}
                        : std::string(name.substr(dot)));
  }
  friend bool operator==(const FilePath&, const FilePath&) = default;
  friend bool operator==(const FilePath& path, std::string_view text) {
    return path.text_ == text;
  }
  friend FilePath operator/(const FilePath& left, std::string_view right) {
    return FilePath(base::JoinPath(left.text_, right));
  }
  friend FilePath operator/(const FilePath& left, const FilePath& right) {
    return FilePath(base::JoinPath(left.text_, right.text_));
  }

 private:
  std::string text_;
};

// Transfers a base status into the error_code convention used by test helpers.
inline bool SetError(base::Status status, std::error_code& error) {
  error = status.ok() ? std::error_code{} : status.system_error;
  if (!status.ok() && !error) {
    error = std::make_error_code(std::errc::invalid_argument);
  }
  return status.ok();
}

inline FilePath CurrentPath(std::error_code& error) {
  std::string result;
  SetError(base::GetWorkingDirectory(result), error);
  return FilePath(std::move(result));
}
inline FilePath CurrentPath() {
  std::error_code error;
  auto result = CurrentPath(error);
  EXPECT_FALSE(error) << error.message();
  return FilePath(std::move(result));
}
inline FilePath AbsolutePath(const FilePath& path, std::error_code& error) {
  error.clear();
  if (!path.is_relative()) {
    return path;
  }
  auto current = CurrentPath(error);
  return error ? FilePath{} : current / path;
}
inline FilePath AbsolutePath(const FilePath& path) {
  std::error_code error;
  auto result = AbsolutePath(path, error);
  EXPECT_FALSE(error) << error.message();
  return FilePath(std::move(result));
}
inline FilePath CanonicalPath(const FilePath& path, std::error_code& error) {
  std::string result;
  SetError(base::CanonicalizePath(path.string(), result), error);
  return FilePath(std::move(result));
}
inline FilePath CanonicalPath(const FilePath& path) {
  std::error_code error;
  auto result = CanonicalPath(path, error);
  EXPECT_FALSE(error) << error.message();
  return FilePath(std::move(result));
}
// Produces a relative spelling between canonical absolute paths.
inline FilePath RelativePath(const FilePath& path, const FilePath& base_path,
                             std::error_code& error) {
  auto target = CanonicalPath(path, error).string();
  if (error) {
    return {};
  }
  auto origin = CanonicalPath(base_path, error).string();
  if (error) {
    return {};
  }
  size_t common = 0;
  while (common < target.size() && common < origin.size() &&
         target[common] == origin[common]) {
    ++common;
  }
  if (common == target.size() && common == origin.size()) {
    return FilePath(".");
  }
  if (common == origin.size() && target[common] == '/') {
    ++common;
  } else if (common != target.size() || origin[common] != '/') {
    while (common > 0 && target[common - 1] != '/') {
      --common;
    }
  }
  std::string result;
  std::string_view remaining(origin);
  remaining.remove_prefix(common > remaining.size() ? remaining.size()
                                                    : common);
  while (!remaining.empty()) {
    result += "../";
    const auto slash = remaining.find('/');
    if (slash == std::string_view::npos) {
      break;
    }
    remaining.remove_prefix(slash + 1);
  }
  result += target.substr(common);
  return FilePath(result.empty() ? "." : result);
}
inline bool CreateDirectories(const FilePath& path, std::error_code& error) {
  bool created = false;
  SetError(base::CreateDirectories(path.string(), created), error);
  return created;
}
inline bool CreateDirectory(const FilePath& path, std::error_code& error) {
  if (::mkdir(path.c_str(), 0700) == 0) {
    error.clear();
    return true;
  }
  error = errno == EEXIST ? std::error_code{}
                          : std::error_code(errno, std::generic_category());
  return false;
}
inline bool CreateDirectory(const FilePath& path) {
  std::error_code error;
  const bool created = CreateDirectory(path, error);
  EXPECT_FALSE(error) << error.message();
  return created;
}
inline void RemoveTree(const FilePath& path, std::error_code& error) {
  SetError(base::RemoveTree(path.string()), error);
}
inline void RemoveTree(const FilePath& path) {
  std::error_code error;
  RemoveTree(path, error);
  EXPECT_FALSE(error) << error.message();
}
inline bool RemoveFile(const FilePath& path, std::error_code& error) {
  bool removed = false;
  SetError(base::RemoveFile(path.string(), removed), error);
  return removed;
}
inline bool RemoveFile(const FilePath& path) {
  std::error_code error;
  const bool removed = RemoveFile(path, error);
  EXPECT_FALSE(error) << error.message();
  return removed;
}
inline bool Exists(const FilePath& path) {
  return base::FileExists(path.string());
}
inline void Rename(const FilePath& source, const FilePath& target,
                   std::error_code& error) {
  error = ::rename(source.c_str(), target.c_str()) == 0
              ? std::error_code{}
              : std::error_code(errno, std::generic_category());
}
inline bool CopyFile(const FilePath& source, const FilePath& target,
                     std::error_code& error) {
  return SetError(base::CopyFile(source.string(), target.string(), false),
                  error);
}
inline bool CopyFile(const FilePath& source, const FilePath& target) {
  std::error_code error;
  const bool copied = CopyFile(source, target, error);
  EXPECT_FALSE(error) << error.message();
  return copied;
}
inline bool OverwriteFile(const FilePath& source, const FilePath& target,
                          std::error_code& error) {
  return SetError(base::CopyFile(source.string(), target.string(), true),
                  error);
}
inline uint64_t FileSize(const FilePath& path) {
  uint64_t result = 0;
  auto status = base::FileSize(path.string(), result);
  EXPECT_TRUE(status.ok()) << status.system_error.message();
  return result;
}
// Matches the directory-entry usage in corruption and persistence tests.
struct FileEntry {
  FilePath name;
  const FilePath& path() const { return name; }
};
inline std::vector<FileEntry> Entries(
    const FilePath& path,
    base::DirectoryTraversal traversal = base::DirectoryTraversal::kDirect) {
  std::vector<std::string> paths;
  auto status = base::ListDirectory(path.string(), paths, traversal);
  EXPECT_TRUE(status.ok()) << status.system_error.message();
  std::vector<FileEntry> entries;
  entries.reserve(paths.size());
  for (auto& entry : paths) {
    entries.push_back({FilePath(std::move(entry))});
  }
  return entries;
}
inline std::vector<FileEntry> RecursiveEntries(const FilePath& path) {
  return Entries(path, base::DirectoryTraversal::kRecursive);
}

}  // namespace yquant::testing::files

#endif  // YQUANT_TESTS_FILE_TEST_SUPPORT_H_
