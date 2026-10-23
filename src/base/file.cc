#include "yquant/base/file.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "yquant/base/path.h"

namespace yquant::base {
namespace {

// Owns a descriptor, including during failures in recursive file operations.
class ScopedFd {
 public:
  explicit ScopedFd(int fd) : fd_(fd) {}
  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
  ~ScopedFd() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }
  int get() const { return fd_; }
  // Releases ownership before close so a failed close is never retried.
  int Close() noexcept { return ::close(std::exchange(fd_, -1)); }

 private:
  int fd_;
};

Status IoError(int number = errno) {
  return {ErrorCode::kIoError, {number, std::generic_category()}};
}

bool ValidPath(std::string_view path) {
  return !path.empty() && path.find('\0') == std::string_view::npos;
}

// Deletes through an opened parent so a replacement symlink cannot redirect
// traversal outside the tree being removed.
Status RemoveEntry(int parent, const char* name) {
  struct stat info{};
  if (::fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT ? Status{} : IoError();
  }
  if (!S_ISDIR(info.st_mode)) {
    if (::unlinkat(parent, name, 0) == 0 || errno == ENOENT) {
      return {};
    }
    return IoError();
  }
  const int fd =
      ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    return IoError();
  }
  DIR* directory = ::fdopendir(fd);
  if (!directory) {
    const auto error = IoError();
    ::close(fd);
    return error;
  }
  Status status;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(directory);
    if (!entry) {
      if (errno != 0) {
        status = IoError();
      }
      break;
    }
    const std::string_view child(entry->d_name);
    if (child == "." || child == "..") {
      continue;
    }
    status = RemoveEntry(fd, entry->d_name);
    if (!status.ok()) {
      break;
    }
  }
  if (::closedir(directory) != 0 && status.ok()) {
    status = IoError();
  }
  if (!status.ok()) {
    return status;
  }
  if (::unlinkat(parent, name, AT_REMOVEDIR) == 0 || errno == ENOENT) {
    return {};
  }
  return IoError();
}

// Enumerates through descriptors; opens child directories without following
// symlinks, even if entries change during enumeration.
Status ReadDirectory(int fd, std::string_view path,
                     DirectoryTraversal traversal,
                     std::vector<std::string>& entries) {
  DIR* directory = ::fdopendir(fd);
  if (!directory) {
    const auto error = IoError();
    ::close(fd);
    return error;
  }
  Status status;
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(directory);
    if (!entry) {
      if (errno != 0) {
        status = IoError();
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    const std::string child = JoinPath(path, name);
    entries.push_back(child);
    switch (traversal) {
      case DirectoryTraversal::kDirect:
        break;
      case DirectoryTraversal::kRecursive: {
        struct stat info{};
        if (::fstatat(fd, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
          status = IoError();
          break;
        }
        if (!S_ISDIR(info.st_mode)) {
          break;
        }
        const int child_fd = ::openat(
            fd, entry->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        status = child_fd < 0
                     ? IoError()
                     : ReadDirectory(child_fd, child, traversal, entries);
        break;
      }
    }
    if (!status.ok()) {
      break;
    }
  }
  if (::closedir(directory) != 0 && status.ok()) {
    status = IoError();
  }
  return status;
}

}  // namespace

DirectoryReader::~DirectoryReader() { Close(); }

Status DirectoryReader::Open(std::string_view path) {
  if (directory_) {
    return {ErrorCode::kInvalidState, {}};
  }
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string text(path);
  const int fd = ::open(text.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  directory_ = ::fdopendir(fd);
  if (!directory_) {
    const auto status = IoError();
    ::close(fd);
    return status;
  }
  return {};
}

Status DirectoryReader::Next(std::string_view& name, bool& done) {
  if (!directory_) {
    return {ErrorCode::kInvalidState, {}};
  }
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(directory_);
    if (!entry) {
      if (errno != 0) {
        return IoError();
      }
      done = true;
      return {};
    }
    const std::string_view candidate(entry->d_name);
    if (candidate == "." || candidate == "..") {
      continue;
    }
    name = candidate;
    done = false;
    return {};
  }
}

void DirectoryReader::Close() noexcept {
  if (directory_) {
    ::closedir(directory_);
  }
  directory_ = nullptr;
}

Status CreateDirectories(std::string_view path, bool& created) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string text(path);
  if (::mkdir(text.c_str(), 0700) == 0) {
    created = true;
    return {};
  }
  const int error = errno;
  if (error == EEXIST) {
    struct stat info{};
    if (::stat(text.c_str(), &info) != 0) {
      return IoError();
    }
    if (!S_ISDIR(info.st_mode)) {
      return IoError(ENOTDIR);
    }
    created = false;
    return {};
  }
  if (error != ENOENT) {
    return IoError(error);
  }
  const std::string parent = ParentPath(path);
  if (parent.empty() || parent == path) {
    return IoError(error);
  }
  bool parent_created = false;
  auto status = CreateDirectories(parent, parent_created);
  if (!status.ok()) {
    return status;
  }
  return CreateDirectories(path, created);
}

Status ListDirectory(std::string_view path, std::vector<std::string>& entries,
                     DirectoryTraversal traversal) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string text(path);
  const int fd = ::open(text.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return IoError();
  }
  std::vector<std::string> candidate;
  auto status = ReadDirectory(fd, path, traversal, candidate);
  if (status.ok()) {
    entries = std::move(candidate);
  }
  return status;
}

Status RemoveFile(std::string_view path, bool& removed) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string text(path);
  if (::remove(text.c_str()) == 0) {
    removed = true;
    return {};
  }
  if (errno != ENOENT) {
    return IoError();
  }
  removed = false;
  return {};
}

Status RemoveTree(std::string_view path) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  std::string text(path);
  while (text.size() > 1 && text.ends_with('/')) {
    text.pop_back();
  }
  const auto name = PathFilename(text);
  if (name.empty() || name == "." || name == "..") {
    return {ErrorCode::kInvalidArgument, {}};
  }
  return RemoveEntry(AT_FDCWD, text.c_str());
}

Status CopyFile(std::string_view source, std::string_view destination,
                bool overwrite) {
  if (!ValidPath(source) || !ValidPath(destination)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string input(source), output(destination);
  ScopedFd in(
      ::open(input.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (in.get() < 0) {
    return IoError();
  }
  struct stat source_info{};
  if (::fstat(in.get(), &source_info) != 0) {
    return IoError();
  }
  if (!S_ISREG(source_info.st_mode)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  ScopedFd out(::open(output.c_str(),
                      O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK |
                          (overwrite ? 0 : O_EXCL),
                      source_info.st_mode & 0777));
  if (out.get() < 0) {
    return IoError();
  }
  struct stat target_info{};
  if (::fstat(out.get(), &target_info) != 0) {
    return IoError();
  }
  if (!S_ISREG(target_info.st_mode) ||
      (source_info.st_dev == target_info.st_dev &&
       source_info.st_ino == target_info.st_ino)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  if (::ftruncate(out.get(), 0) != 0) {
    return IoError();
  }
  std::array<char, 16384> buffer;
  for (;;) {
    const ssize_t count = ::read(in.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return IoError();
    }
    if (count == 0) {
      if (out.Close() != 0) {
        return IoError();
      }
      if (in.Close() != 0) {
        return IoError();
      }
      return {};
    }
    ssize_t offset = 0;
    while (offset < count) {
      const ssize_t written =
          ::write(out.get(), buffer.data() + offset, count - offset);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return IoError();
      }
      if (written == 0) {
        return IoError(EIO);
      }
      offset += written;
    }
  }
}

Status FileSize(std::string_view path, uint64_t& size) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  const std::string text(path);
  struct stat info{};
  if (::stat(text.c_str(), &info) != 0) {
    return IoError();
  }
  if (!S_ISREG(info.st_mode)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  size = static_cast<uint64_t>(info.st_size);
  return {};
}

bool FileExists(std::string_view path) {
  if (!ValidPath(path)) {
    return false;
  }
  const std::string text(path);
  struct stat info{};
  return ::stat(text.c_str(), &info) == 0;
}

}  // namespace yquant::base
