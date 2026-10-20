#ifndef YQUANT_STRATEGYD_JSON_CONFIG_DOCUMENT_H_
#define YQUANT_STRATEGYD_JSON_CONFIG_DOCUMENT_H_

#include <cstddef>
#include <string_view>

#include "yquant/base/status.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxConfigFileBytes = 1024 * 1024;
inline constexpr std::size_t kMaxConfigDepth = 64;

// Owns bounded UTF-8 JSON object source, validated before schema parsing.
// Rejects malformed values, excess depth and decoded duplicate keys at every
// level. Temporary parser/key storage is released before Load() returns.
class JsonConfigDocument {
 public:
  JsonConfigDocument() noexcept = default;
  ~JsonConfigDocument() noexcept;
  JsonConfigDocument(const JsonConfigDocument&) = delete;
  JsonConfigDocument& operator=(const JsonConfigDocument&) = delete;

  // Replaces the previous document. On failure source() is empty and error()
  // owns the diagnostic. path is borrowed only during the call.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  // Releases source and invalidates its views; preserves the last diagnostic.
  void Reset() noexcept;

  std::string_view source() const noexcept { return {source_, size_}; }
  // Allocated source bytes including padding for schema-specific parsers.
  std::size_t capacity() const noexcept { return capacity_; }
  const strategy::ConfigError& error() const noexcept { return error_; }

 private:
  char* source_ = nullptr;
  std::size_t size_ = 0;
  std::size_t capacity_ = 0;
  strategy::ConfigError error_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_JSON_CONFIG_DOCUMENT_H_
