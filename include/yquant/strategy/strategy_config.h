#ifndef YQUANT_STRATEGY_STRATEGY_CONFIG_H_
#define YQUANT_STRATEGY_STRATEGY_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::strategy {

inline constexpr std::size_t kMaxStrategyIdBytes = 64;

// Owns the decoded UTF-8 bytes. IDs are never truncated or normalized.
class StrategyId {
 public:
  [[nodiscard]] base::ErrorCode Assign(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxStrategyIdBytes ||
        !IsValidUtf8(value)) {
      return base::ErrorCode::kInvalidArgument;
    }
    std::memmove(value_, value.data(), value.size());
    size_ = value.size();
    return base::ErrorCode::kOk;
  }

  std::string_view view() const noexcept { return {value_, size_}; }

 private:
  static bool IsValidUtf8(std::string_view value) noexcept {
    std::size_t index = 0;
    while (index < value.size()) {
      const auto first = static_cast<std::uint8_t>(value[index]);
      if (first == 0) {
        return false;
      }
      if (first <= 0x7f) {
        ++index;
        continue;
      }

      std::size_t length;
      std::uint32_t code_point;
      std::uint32_t minimum;
      if (first >= 0xc2 && first <= 0xdf) {
        length = 2;
        code_point = first & 0x1f;
        minimum = 0x80;
      } else if (first >= 0xe0 && first <= 0xef) {
        length = 3;
        code_point = first & 0x0f;
        minimum = 0x800;
      } else if (first >= 0xf0 && first <= 0xf4) {
        length = 4;
        code_point = first & 0x07;
        minimum = 0x10000;
      } else {
        return false;
      }

      if (length > value.size() - index) {
        return false;
      }
      for (std::size_t offset = 1; offset < length; ++offset) {
        const auto next = static_cast<std::uint8_t>(value[index + offset]);
        if ((next & 0xc0) != 0x80) {
          return false;
        }
        code_point = (code_point << 6) | (next & 0x3f);
      }
      if (code_point < minimum || code_point > 0x10ffff ||
          (code_point >= 0xd800 && code_point <= 0xdfff)) {
        return false;
      }
      index += length;
    }
    return true;
  }

  char value_[kMaxStrategyIdBytes] = {};
  std::size_t size_ = 0;
};

// Diagnostic strings remain valid after plugin objects and libraries are gone.
// Parameter loaders report paths relative to the parameters object.
struct ConfigError {
  char field_path[256] = {};
  char message[512] = {};
  bool truncated = false;

  void Clear() noexcept {
    field_path[0] = '\0';
    message[0] = '\0';
    truncated = false;
  }

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

class StrategyConfig {
 public:
  StrategyConfig() noexcept = default;
  virtual ~StrategyConfig() noexcept = default;

  StrategyConfig(const StrategyConfig&) = delete;
  StrategyConfig& operator=(const StrategyConfig&) = delete;
  StrategyConfig(StrategyConfig&&) = delete;
  StrategyConfig& operator=(StrategyConfig&&) = delete;

  std::string_view strategy_id() const noexcept { return strategy_id_.view(); }
  bool ready() const noexcept { return ready_; }

  // One attempt per instance, including failures. JSON is borrowed only for the
  // duration of this call; the host has already validated its object syntax.
  [[nodiscard]] base::ErrorCode Initialize(const StrategyId& strategy_id,
                                           std::string_view parameters_json,
                                           ConfigError* error) noexcept {
    if (attempted_) {
      return base::ErrorCode::kInvalidState;
    }
    attempted_ = true;
    if (error == nullptr) {
      return base::ErrorCode::kInvalidArgument;
    }
    error->Clear();
    if (strategy_id.view().empty()) {
      error->Set("strategy_id", "strategy ID must not be empty");
      return base::ErrorCode::kInvalidArgument;
    }
    strategy_id_ = strategy_id;
    const auto result = LoadParameters(parameters_json, error);
    ready_ = result == base::ErrorCode::kOk;
    return result;
  }

 protected:
  // Own all retained values and report errors through the supplied storage.
  // The default configuration accepts only an empty object.
  [[nodiscard]] virtual base::ErrorCode LoadParameters(
      std::string_view parameters_json, ConfigError* error) noexcept {
    std::size_t index = 0;
    auto skip_whitespace = [&]() noexcept {
      while (index < parameters_json.size()) {
        const char value = parameters_json[index];
        if (value != ' ' && value != '\t' && value != '\n' && value != '\r') {
          break;
        }
        ++index;
      }
    };
    skip_whitespace();
    if (index < parameters_json.size() && parameters_json[index] == '{') {
      ++index;
      skip_whitespace();
      if (index < parameters_json.size() && parameters_json[index] == '}') {
        ++index;
        skip_whitespace();
        if (index == parameters_json.size()) {
          return base::ErrorCode::kOk;
        }
      }
    }
    error->Set("", "expected an empty parameters object");
    return base::ErrorCode::kInvalidArgument;
  }

 private:
  StrategyId strategy_id_;
  bool attempted_ = false;
  bool ready_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_CONFIG_H_
