// Defines strategy IDs, copied diagnostics, and the typed configuration base.
// JSON parsing and file I/O stay in the host and individual strategy modules.

#ifndef YQUANT_STRATEGY_STRATEGY_CONFIG_H_
#define YQUANT_STRATEGY_STRATEGY_CONFIG_H_

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"

namespace yquant::strategy {

inline constexpr std::size_t kMaxStrategyIdBytes = 64;

// Owns decoded UTF-8 bytes for a strategy ID. Initially empty, it retains each
// accepted ID without truncation or normalization.
class StrategyId {
 public:
  // Copies value if it contains 1 to 64 bytes of valid UTF-8 without NUL.
  // Returns kInvalidArgument and preserves the previous ID otherwise.
  // Retains no reference to value.
  [[nodiscard]] base::ErrorCode Assign(std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaxStrategyIdBytes ||
        !IsValidUtf8(value)) {
      return base::ErrorCode::kInvalidArgument;
    }
    std::memmove(value_, value.data(), value.size());
    size_ = value.size();
    return base::ErrorCode::kOk;
  }

  // Borrows this object's storage until the ID is replaced or the object is
  // destroyed. The view is not guaranteed to be NUL-terminated.
  std::string_view view() const noexcept { return {value_, size_}; }

 private:
  // Validates UTF-8 scalar encodings and rejects NUL for use in strategy IDs.
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
      // Require the shortest encoding and exclude surrogate halves, which are
      // not Unicode scalar values.
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

// Strategy parameter diagnostics use paths relative to parameters. Keep the
// SDK spelling while sharing the owned diagnostic representation with hosts.
using ConfigError = base::ConfigError;

// Holds common fields and initializes a strategy's typed parameters.
// The strategy exposes this embedded object through config(); the host
// initializes it before OnStart().
class StrategyConfig {
 public:
  StrategyConfig() noexcept = default;
  virtual ~StrategyConfig() noexcept = default;

  StrategyConfig(const StrategyConfig&) = delete;
  StrategyConfig& operator=(const StrategyConfig&) = delete;
  StrategyConfig(StrategyConfig&&) = delete;
  StrategyConfig& operator=(StrategyConfig&&) = delete;

  // Borrows this object's ID storage. The ID is empty until copied during
  // initialization and may remain populated if parameter loading fails.
  std::string_view strategy_id() const noexcept { return strategy_id_.view(); }

  // Returns whether initialization succeeded. Use the complete configuration
  // only when true; successful values remain read-only.
  bool ready() const noexcept { return ready_; }

  // Allows only one initialization attempt, even if it fails.
  // The caller must prevent concurrent access during initialization.
  // Borrows validated JSON object text and error for this call only.
  // Clears error before validation. Returns kInvalidArgument for an empty ID,
  // otherwise the loader's result. Retries return kInvalidState and leave this
  // object and error unchanged.
  [[nodiscard]] base::ErrorCode Initialize(const StrategyId& strategy_id,
                                           std::string_view parameters_json,
                                           ConfigError& error) noexcept {
    if (attempted_) {
      return base::ErrorCode::kInvalidState;
    }
    // Consume the attempt before validation so failed inputs cannot be retried.
    attempted_ = true;
    error.Clear();
    if (strategy_id.view().empty()) {
      error.Set("strategy_id", "strategy ID must not be empty");
      return base::ErrorCode::kInvalidArgument;
    }
    // Make the common ID available to the parameter loader, even on failure.
    strategy_id_ = strategy_id;
    const auto result = LoadParameters(parameters_json, error);
    ready_ = result == base::ErrorCode::kOk;
    return result;
  }

 protected:
  // Loads typed parameters after the ID is set; the base accepts only {}.
  // Overrides reject unknown fields and validate required fields and values.
  // Borrows JSON and error for this call; retain only owned data.
  // Returns kOk on success. On failure, writes a relative field path and reason
  // to error and returns an error code. Failed objects must be safe to destroy.
  [[nodiscard]] virtual base::ErrorCode LoadParameters(
      std::string_view parameters_json, ConfigError& error) noexcept {
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
    error.Set("", "expected an empty parameters object");
    return base::ErrorCode::kInvalidArgument;
  }

 private:
  StrategyId strategy_id_;
  // Records an attempt regardless of its outcome; ready_ records only success.
  bool attempted_ = false;
  bool ready_ = false;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_CONFIG_H_
