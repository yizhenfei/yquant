#ifndef YQUANT_BASE_CONFIG_ERROR_H_
#define YQUANT_BASE_CONFIG_ERROR_H_

#include <cstring>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::base {

// Owns configuration diagnostics independently of input and parser storage.
struct ConfigError {
  // A field path relative to the schema root; empty means the object.
  char field_path[256] = {};
  char message[512] = {};
  // Indicates omitted diagnostic text, including an already truncated path.
  bool truncated = false;

  void Clear() noexcept {
    field_path[0] = '\0';
    message[0] = '\0';
    truncated = false;
  }

  // Replaces both strings with NUL-terminated copies, retaining no input views.
  // Resets truncated to whether either input was shortened to fit its buffer.
  void Set(std::string_view path, std::string_view reason) noexcept {
    const auto path_size =
        path.size() < sizeof(field_path) ? path.size() : sizeof(field_path) - 1;
    const auto message_size =
        reason.size() < sizeof(message) ? reason.size() : sizeof(message) - 1;
    if (path_size != 0) {
      std::memmove(field_path, path.data(), path_size);
    }
    field_path[path_size] = '\0';
    if (message_size != 0) {
      std::memmove(message, reason.data(), message_size);
    }
    message[message_size] = '\0';
    truncated = path_size != path.size() || message_size != reason.size();
  }
};

// Copies a validation diagnostic and returns kInvalidArgument. Retains no
// views.
[[nodiscard]] inline Status InvalidConfig(std::string_view field,
                                          std::string_view reason,
                                          ConfigError& error) noexcept {
  error.Set(field, reason);
  return {ErrorCode::kInvalidArgument, {}};
}

}  // namespace yquant::base

#endif  // YQUANT_BASE_CONFIG_ERROR_H_
