#include "strategyd/strategy_config_loader.h"

#include <simdjson.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <utility>

namespace yquant::strategyd {
namespace {

using JsonType = simdjson::ondemand::json_type;

base::Status JsonError(simdjson::error_code code, std::string_view path,
                       strategy::ConfigError* error) noexcept {
  error->Set(path, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

base::Status Invalid(std::string_view path, std::string_view reason,
                     strategy::ConfigError* error) noexcept {
  error->Set(path, reason);
  return {base::ErrorCode::kInvalidArgument, {}};
}

bool IsJsonSpace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

std::string_view TrimEnd(std::string_view value) noexcept {
  while (!value.empty() && IsJsonSpace(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

struct ConfigPath {
  char text[sizeof(strategy::ConfigError::field_path)] = {};
  std::size_t size = 0;
  bool truncated = false;

  void Append(std::string_view value) noexcept {
    const std::size_t remaining = sizeof(text) - 1 - size;
    const std::size_t copied =
        value.size() < remaining ? value.size() : remaining;
    if (copied != 0) {
      std::memcpy(text + size, value.data(), copied);
    }
    size += copied;
    text[size] = '\0';
    truncated |= copied != value.size();
  }

  std::string_view view() const noexcept { return {text, size}; }
};

ConfigPath FieldPath(std::string_view parent, std::string_view key) noexcept {
  ConfigPath path;
  path.Append(parent);
  if (!parent.empty()) {
    path.Append(".");
  }
  for (const unsigned char byte : key) {
    if (byte < 0x20 || byte == 0x7f) {
      constexpr char kHex[] = "0123456789abcdef";
      const char escaped[] = {
          '\\', 'u', '0', '0', kHex[byte >> 4], kHex[byte & 0xf]};
      path.Append({escaped, sizeof(escaped)});
    } else {
      const char character = static_cast<char>(byte);
      path.Append({&character, 1});
    }
  }
  return path;
}

ConfigPath IndexPath(std::string_view parent, std::size_t index) noexcept {
  ConfigPath path;
  path.Append(parent);
  char suffix[32];
  const int length = std::snprintf(suffix, sizeof(suffix), "[%zu]", index);
  path.Append({suffix, static_cast<std::size_t>(length)});
  return path;
}

// Validate number syntax without imposing an integer width or floating-point
// range. The strategy parses the unchanged token into its own chosen type.
bool IsJsonNumber(std::string_view value) noexcept {
  value = TrimEnd(value);
  std::size_t offset = 0;
  if (offset < value.size() && value[offset] == '-') {
    ++offset;
  }
  if (offset == value.size()) {
    return false;
  }
  if (value[offset] == '0') {
    ++offset;
  } else {
    if (value[offset] < '1' || value[offset] > '9') {
      return false;
    }
    do {
      ++offset;
    } while (offset < value.size() && value[offset] >= '0' &&
             value[offset] <= '9');
  }
  if (offset < value.size() && value[offset] == '.') {
    const std::size_t fraction_start = ++offset;
    while (offset < value.size() && value[offset] >= '0' &&
           value[offset] <= '9') {
      ++offset;
    }
    if (offset == fraction_start) {
      return false;
    }
  }
  if (offset < value.size() && (value[offset] == 'e' || value[offset] == 'E')) {
    ++offset;
    if (offset < value.size() &&
        (value[offset] == '+' || value[offset] == '-')) {
      ++offset;
    }
    const std::size_t exponent_start = offset;
    while (offset < value.size() && value[offset] >= '0' &&
           value[offset] <= '9') {
      ++offset;
    }
    if (offset == exponent_start) {
      return false;
    }
  }
  return offset == value.size();
}

struct KeyEntry {
  // Empty slots have key_data == nullptr; even an empty decoded key has a
  // non-null pointer into simdjson's string storage.
  const char* key_data;
  std::size_t key_size;
  std::size_t object_id;
};

class JsonValidator {
 public:
  explicit JsonValidator(strategy::ConfigError* error) noexcept
      : error_(error) {}
  ~JsonValidator() noexcept { std::free(keys_); }

  JsonValidator(const JsonValidator&) = delete;
  JsonValidator& operator=(const JsonValidator&) = delete;

  base::Status Allocate(std::size_t input_size) noexcept {
    // Every member consumes at least four distinct source bytes: two quotes,
    // a colon, and at least one value byte. Keep the table below half full.
    const std::size_t member_bound = input_size / 4;
    const std::size_t min_capacity = member_bound == 0 ? 2 : 2 * member_bound;
    key_capacity_ = 1;
    while (key_capacity_ < min_capacity) {
      key_capacity_ *= 2;
    }
    keys_ =
        static_cast<KeyEntry*>(std::calloc(key_capacity_, sizeof(KeyEntry)));
    if (keys_ == nullptr) {
      error_->Set("$", "cannot allocate duplicate-key validation storage");
      return {base::ErrorCode::kResourceExhausted, {}};
    }
    return {};
  }

  base::Status ValidateRoot(simdjson::ondemand::object object,
                            strategy::StrategyId* strategy_id) noexcept {
    const std::size_t object_id = next_object_id_++;
    bool has_id = false;
    bool has_parameters = false;
    for (auto field_result : object) {
      simdjson::ondemand::field field;
      simdjson::error_code json_error = std::move(field_result).get(field);
      if (json_error) {
        return JsonError(json_error, "$", error_);
      }
      std::string_view key;
      json_error = field.unescaped_key().get(key);
      if (json_error) {
        return JsonError(json_error, "$", error_);
      }
      if (!InsertKey(object_id, key)) {
        const ConfigPath path = FieldPath({}, key);
        const auto result =
            Invalid(path.view(), "duplicate object key", error_);
        error_->truncated |= path.truncated;
        return result;
      }
      if (key == "strategy_id") {
        std::string_view value;
        json_error = field.value().get_string().get(value);
        if (json_error) {
          return JsonError(json_error, "strategy_id", error_);
        }
        const base::ErrorCode result = strategy_id->Assign(value);
        if (result != base::ErrorCode::kOk) {
          error_->Set("strategy_id",
                      "must contain 1 to 64 UTF-8 bytes without NUL");
          return {result, {}};
        }
        has_id = true;
      } else if (key == "parameters") {
        JsonType type;
        json_error = field.value().type().get(type);
        if (json_error) {
          return JsonError(json_error, "parameters", error_);
        }
        if (type != JsonType::object) {
          return Invalid("parameters", "must be a JSON object", error_);
        }
        const base::Status result =
            ValidateValue(field.value(), "parameters", 2);
        if (!result.ok()) {
          return result;
        }
        has_parameters = true;
      } else {
        const ConfigPath path = FieldPath({}, key);
        const auto result = Invalid(path.view(), "unknown root field", error_);
        error_->truncated |= path.truncated;
        return result;
      }
    }
    if (!has_id) {
      return Invalid("strategy_id", "required field is missing", error_);
    }
    if (!has_parameters) {
      return Invalid("parameters", "required field is missing", error_);
    }
    return {};
  }

 private:
  bool InsertKey(std::size_t object_id, std::string_view key) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : key) {
      hash = (hash ^ byte) * 1099511628211ULL;
    }
    hash = (hash ^ object_id) * 1099511628211ULL;
    std::size_t slot = hash & (key_capacity_ - 1);
    for (;;) {
      KeyEntry& entry = keys_[slot];
      if (entry.key_data == nullptr) {
        entry = {key.data(), key.size(), object_id};
        return true;
      }
      if (entry.object_id == object_id &&
          std::string_view(entry.key_data, entry.key_size) == key) {
        return false;
      }
      slot = (slot + 1) & (key_capacity_ - 1);
    }
  }

  base::Status ValidateValue(simdjson::ondemand::value value,
                             std::string_view path,
                             std::size_t depth) noexcept {
    JsonType type;
    simdjson::error_code json_error = value.type().get(type);
    if (json_error) {
      return JsonError(json_error, path, error_);
    }
    if ((type == JsonType::object || type == JsonType::array) &&
        depth > kMaxConfigDepth) {
      return Invalid(path, "maximum JSON nesting depth is 64", error_);
    }
    switch (type) {
      case JsonType::object: {
        simdjson::ondemand::object object;
        json_error = value.get_object().get(object);
        if (json_error) {
          return JsonError(json_error, path, error_);
        }
        const std::size_t object_id = next_object_id_++;
        for (auto field_result : object) {
          simdjson::ondemand::field field;
          json_error = std::move(field_result).get(field);
          if (json_error) {
            return JsonError(json_error, path, error_);
          }
          std::string_view key;
          json_error = field.unescaped_key().get(key);
          if (json_error) {
            return JsonError(json_error, path, error_);
          }
          const ConfigPath child_path = FieldPath(path, key);
          if (!InsertKey(object_id, key)) {
            const auto result =
                Invalid(child_path.view(), "duplicate object key", error_);
            error_->truncated |= child_path.truncated;
            return result;
          }
          const base::Status result =
              ValidateValue(field.value(), child_path.view(), depth + 1);
          if (!result.ok()) {
            error_->truncated |= child_path.truncated;
            return result;
          }
        }
        return {};
      }
      case JsonType::array: {
        simdjson::ondemand::array array;
        json_error = value.get_array().get(array);
        if (json_error) {
          return JsonError(json_error, path, error_);
        }
        std::size_t index = 0;
        for (auto value_result : array) {
          simdjson::ondemand::value child;
          json_error = std::move(value_result).get(child);
          if (json_error) {
            return JsonError(json_error, path, error_);
          }
          const ConfigPath child_path = IndexPath(path, index++);
          const base::Status result =
              ValidateValue(child, child_path.view(), depth + 1);
          if (!result.ok()) {
            error_->truncated |= child_path.truncated;
            return result;
          }
        }
        return {};
      }
      case JsonType::string: {
        std::string_view decoded;
        json_error = value.get_string().get(decoded);
        break;
      }
      case JsonType::boolean: {
        bool decoded;
        json_error = value.get_bool().get(decoded);
        break;
      }
      case JsonType::null: {
        bool is_null;
        json_error = value.is_null().get(is_null);
        break;
      }
      case JsonType::number:
        if (!IsJsonNumber(value.raw_json_token())) {
          return Invalid(path, "invalid JSON number", error_);
        }
        // The enclosing iterator consumes this validated scalar on increment.
        return {};
      case JsonType::unknown:
        return Invalid(path, "invalid JSON value", error_);
    }
    return json_error ? JsonError(json_error, path, error_) : base::Status{};
  }

  strategy::ConfigError* error_;
  KeyEntry* keys_ = nullptr;
  std::size_t key_capacity_ = 0;
  std::size_t next_object_id_ = 1;
};

base::Status Validate(char* source, std::size_t size, std::size_t capacity,
                      strategy::StrategyId* strategy_id,
                      std::string_view* parameters,
                      strategy::ConfigError* error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::error_code json_error = parser.allocate(size, kMaxConfigDepth + 4);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  simdjson::ondemand::document document;
  json_error = parser.iterate(source, size, capacity).get(document);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  simdjson::ondemand::object root;
  json_error = document.get_object().get(root);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  JsonValidator validator(error);
  base::Status result = validator.Allocate(size);
  if (!result.ok()) {
    return result;
  }
  result = validator.ValidateRoot(root, strategy_id);
  if (!result.ok()) {
    return result;
  }
  if (!document.at_end()) {
    return Invalid("$", "unexpected content after root object", error);
  }

  // Rewind only after every string and token has been validated. raw_json()
  // then locates the object without reserializing any numbers or escapes.
  document.rewind();
  json_error = document.get_object().get(root);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  for (auto field : root) {
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) {
      return JsonError(json_error, "$", error);
    }
    if (key == "parameters") {
      json_error = field.value().raw_json().get(*parameters);
      if (json_error) {
        return JsonError(json_error, "parameters", error);
      }
      *parameters = TrimEnd(*parameters);
      return {};
    }
  }
  return Invalid("parameters", "required field is missing", error);
}

}  // namespace

StrategyConfigLoader::~StrategyConfigLoader() noexcept { Reset(); }

void StrategyConfigLoader::Reset() noexcept {
  std::free(source_);
  source_ = nullptr;
  strategy_id_ = {};
  parameters_json_ = {};
}

base::Status StrategyConfigLoader::Load(const char* path) noexcept {
  Reset();
  error_.Clear();
  if (path == nullptr || path[0] == '\0') {
    return Invalid("$", "configuration file path must not be empty", &error_);
  }
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    const int saved_errno = errno;
    error_.Set("$", "cannot open configuration file");
    return {saved_errno == ENOMEM ? base::ErrorCode::kResourceExhausted
                                  : base::ErrorCode::kIoError,
            {saved_errno, std::generic_category()}};
  }
  // One extra byte detects oversized input; padding is required by simdjson.
  constexpr std::size_t kSourceCapacity =
      kMaxConfigFileBytes + 1 + simdjson::SIMDJSON_PADDING;
  source_ = static_cast<char*>(std::malloc(kSourceCapacity));
  if (source_ == nullptr) {
    (void)std::fclose(file);
    error_.Set("$", "cannot allocate configuration file buffer");
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  const std::size_t size =
      std::fread(source_, 1, kMaxConfigFileBytes + 1, file);
  const bool read_error = std::ferror(file) != 0;
  const int read_errno = errno;
  const int close_result = std::fclose(file);
  const int close_errno = errno;
  if (read_error || close_result != 0) {
    Reset();
    error_.Set("$", "cannot read configuration file");
    return {base::ErrorCode::kIoError,
            {read_error ? read_errno : close_errno, std::generic_category()}};
  }
  if (size > kMaxConfigFileBytes) {
    Reset();
    return Invalid("$", "configuration file exceeds the 1 MiB limit", &error_);
  }
  std::memset(source_ + size, 0, simdjson::SIMDJSON_PADDING);
  const base::Status result =
      Validate(source_, size, kSourceCapacity, &strategy_id_, &parameters_json_,
               &error_);
  if (!result.ok()) {
    Reset();
  }
  return result;
}

}  // namespace yquant::strategyd
