#ifndef YQUANT_BASE_FILE_H_
#define YQUANT_BASE_FILE_H_

// Cold-path POSIX file operations shared by startup and persistence code.
// Paths must be nonempty and contain no NUL. These functions may allocate and
// perform I/O; callers must serialize changes to the process working directory.

#include <dirent.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "yquant/base/status.h"

namespace yquant::base {

// Controls whether directory enumeration visits subdirectories. Symlinks are
// returned as entries and never traversed.
enum class DirectoryTraversal { kDirect, kRecursive };

// Owns one cold-path directory stream. Single-thread-owned; Next returns a
// name valid only until the next Next/Close call. Does not allocate per entry.
class DirectoryReader final {
 public:
  DirectoryReader() = default;
  DirectoryReader(const DirectoryReader&) = delete;
  DirectoryReader& operator=(const DirectoryReader&) = delete;
  ~DirectoryReader();
  // Requires closed state. Resolves the caller-selected root, including a
  // root symlink; entries are not traversed by this reader.
  [[nodiscard]] Status Open(std::string_view path);
  // Skips dot entries; sets done at EOF and preserves name on EOF or failure.
  [[nodiscard]] Status Next(std::string_view& name, bool& done);
  void Close() noexcept;

 private:
  DIR* directory_ = nullptr;
};

// Creates missing parent directories with mode 0700. Sets created to whether
// this call created the final directory. Existing directories are accepted.
[[nodiscard]] Status CreateDirectories(std::string_view path, bool& created);
// Replaces entries with full paths, excluding dot entries. Failure preserves
// entries. Resolves the caller-selected root; recursive traversal never
// follows directory symlinks discovered beneath it.
[[nodiscard]] Status ListDirectory(
    std::string_view path, std::vector<std::string>& entries,
    DirectoryTraversal traversal = DirectoryTraversal::kDirect);
// Removes a file, symlink, or empty directory; absence succeeds. Sets removed.
[[nodiscard]] Status RemoveFile(std::string_view path, bool& removed);
// Removes a tree using descriptor-relative operations without following
// symlinks. Absence succeeds. Failure may leave a partially removed tree.
[[nodiscard]] Status RemoveTree(std::string_view path);
// Copies regular-file bytes; overwrite permits truncating an existing regular
// destination. Rejects symlinks and copying a file onto itself. Failure may
// leave a partial destination.
[[nodiscard]] Status CopyFile(std::string_view source,
                              std::string_view destination, bool overwrite);
// Writes regular-file size in bytes. Failure preserves size.
[[nodiscard]] Status FileSize(std::string_view path, uint64_t& size);
// Returns whether stat can resolve path. A dangling symlink does not exist.
bool FileExists(std::string_view path);

}  // namespace yquant::base

#endif  // YQUANT_BASE_FILE_H_
