#include "yquant/base/path.h"

#include <cstring>
#include <filesystem>

namespace yquant::base {
Status ResolvePath(std::string_view path, std::span<char> out) noexcept {
  if (path.empty() || path.find('\0') != std::string_view::npos)
    return {ErrorCode::kInvalidArgument, {}};
  std::filesystem::path resolved(path);
  if (resolved.is_relative()) {
    std::error_code error;
    resolved = std::filesystem::absolute(resolved, error);
    if (error) return {ErrorCode::kIoError, error};
  }
  const auto& text = resolved.native();
  if (text.size() >= out.size()) return {ErrorCode::kInvalidArgument, {}};
  std::memcpy(out.data(), text.c_str(), text.size() + 1);
  return {};
}
}  // namespace yquant::base
