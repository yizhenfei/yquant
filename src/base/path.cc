#include "yquant/base/path.h"

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace yquant::base {
namespace {

Status IoError(int number = errno) {
  return {ErrorCode::kIoError, {number, std::generic_category()}};
}

bool ValidPath(std::string_view path) {
  return !path.empty() && path.find('\0') == std::string_view::npos;
}

}  // namespace

std::string JoinPath(std::string_view left, std::string_view right) {
  if (left.empty() || right.starts_with('/')) {
    return std::string(right);
  }
  std::string result(left);
  if (!result.ends_with('/')) {
    result += '/';
  }
  result += right;
  return result;
}

std::string_view PathFilename(std::string_view path) noexcept {
  const auto slash = path.find_last_of('/');
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

std::string ParentPath(std::string_view path) {
  while (path.size() > 1 && path.ends_with('/')) {
    path.remove_suffix(1);
  }
  auto slash = path.find_last_of('/');
  if (slash == std::string_view::npos) {
    return {};
  }
  while (slash > 0 && path[slash - 1] == '/') {
    --slash;
  }
  return std::string(path.substr(0, slash == 0 ? 1 : slash));
}

std::string NormalizePath(std::string_view path) {
  if (path.empty()) {
    return {};
  }
  const bool absolute = path.starts_with('/');
  std::vector<std::string_view> parts;
  while (!path.empty()) {
    const auto slash = path.find('/');
    const auto part = path.substr(0, slash);
    if (part == "..") {
      if (!parts.empty() && parts.back() != "..") {
        parts.pop_back();
      } else if (!absolute) {
        parts.push_back(part);
      }
    } else if (!part.empty() && part != ".") {
      parts.push_back(part);
    }
    if (slash == std::string_view::npos) {
      break;
    }
    path.remove_prefix(slash + 1);
  }
  std::string result = absolute ? "/" : "";
  for (const auto part : parts) {
    if (!result.empty() && !result.ends_with('/')) {
      result += '/';
    }
    result += part;
  }
  return result.empty() ? "." : result;
}

Status GetWorkingDirectory(std::string& output) {
  std::vector<char> buffer(256);
  while (!::getcwd(buffer.data(), buffer.size())) {
    if (errno != ERANGE) {
      return IoError();
    }
    buffer.resize(buffer.size() * 2);
  }
  output.assign(buffer.data());
  return {};
}

Status CanonicalizePath(std::string_view path, std::string& output) {
  if (!ValidPath(path)) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  std::string prefix(path);
  if (!path.starts_with('/')) {
    std::string current;
    auto status = GetWorkingDirectory(current);
    if (!status.ok()) {
      return status;
    }
    prefix = JoinPath(current, path);
  }
  std::vector<std::string> missing;
  for (;;) {
    char* resolved = ::realpath(prefix.c_str(), nullptr);
    if (resolved) {
      std::string result(resolved);
      std::free(resolved);
      for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        result = JoinPath(result, *it);
      }
      output = NormalizePath(result);
      return {};
    }
    if (errno != ENOENT && errno != ENOTDIR) {
      return IoError();
    }
    while (prefix.size() > 1 && prefix.ends_with('/')) {
      prefix.pop_back();
    }
    missing.emplace_back(PathFilename(prefix));
    const std::string parent = ParentPath(prefix);
    if (parent.empty() || parent == prefix) {
      return IoError();
    }
    prefix = parent;
  }
}

Status ResolvePath(std::string_view path, std::span<char> out) noexcept {
  if (path.empty() || path.find('\0') != std::string_view::npos) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  std::string text(path);
  if (!path.starts_with('/')) {
    std::string current;
    const auto status = GetWorkingDirectory(current);
    if (!status.ok()) {
      return status;
    }
    text = JoinPath(current, path);
  }
  if (text.size() >= out.size()) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  std::memcpy(out.data(), text.c_str(), text.size() + 1);
  return {};
}
}  // namespace yquant::base
