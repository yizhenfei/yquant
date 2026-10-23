#include "yquant/base/json_config_document.h"

#include <stddef.h>
#include <stdint.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "simdjson.h"
#include "yquant/base/format.h"

namespace yquant::base {
namespace {

using JsonType = simdjson::ondemand::json_type;

using internal::JsonError;

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
  char text[sizeof(ConfigError::field_path)] = {};
  size_t size = 0;
  bool truncated = false;

  // Appends what fits and remembers truncation across subsequent components.
  void Append(std::string_view value) noexcept {
    const size_t remaining = sizeof(text) - 1 - size;
    const size_t copied = value.size() < remaining ? value.size() : remaining;
    if (copied != 0) {
      std::memcpy(text + size, value.data(), copied);
    }
    size += copied;
    text[size] = '\0';
    truncated |= copied != value.size();
  }

  std::string_view view() const noexcept { return {text, size}; }
};

// Escapes control bytes so diagnostics cannot hide key suffixes behind NUL.
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

// Appends an array index without allocating; 32 bytes fit any 64-bit index.
ConfigPath IndexPath(std::string_view parent, size_t index) noexcept {
  ConfigPath path;
  path.Append(parent);
  char suffix[32];
  const int length =
      ::yquant::base::FormatTo(suffix, sizeof(suffix), "[{}]", index);
  path.Append({suffix, static_cast<size_t>(length)});
  return path;
}

// Validate number syntax without imposing an integer width or floating-point
// range. The schema parser converts the unchanged token to its chosen type.
bool IsJsonNumber(std::string_view value) noexcept {
  value = TrimEnd(value);
  size_t offset = 0;
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
    const size_t fraction_start = ++offset;
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
    const size_t exponent_start = offset;
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
  size_t key_size;
  size_t object_id;
};

// Traverses one parser document, retaining decoded key views only while that
// parser lives. The preallocated table distinguishes keys by owning object.
class JsonValidator {
 public:
  // Borrows an error object that outlives the complete traversal.
  explicit JsonValidator(ConfigError& error) noexcept : error_(error) {}
  ~JsonValidator() noexcept { std::free(keys_); }

  JsonValidator(const JsonValidator&) = delete;
  JsonValidator& operator=(const JsonValidator&) = delete;

  // Allocates once using a source-size bound. Failure remains safe to destroy.
  Status Allocate(size_t input_size) noexcept {
    // Every member consumes at least four distinct source bytes: two quotes,
    // a colon, and at least one value byte. Keep the table below half full.
    const size_t member_bound = input_size / 4;
    const size_t min_capacity = member_bound == 0 ? 2 : 2 * member_bound;
    key_capacity_ = 1;
    while (key_capacity_ < min_capacity) {
      key_capacity_ *= 2;
    }
    keys_ =
        static_cast<KeyEntry*>(std::calloc(key_capacity_, sizeof(KeyEntry)));
    if (keys_ == nullptr) {
      error_.Set("$", "cannot allocate duplicate-key validation storage");
      return {ErrorCode::kResourceExhausted, {}};
    }
    return {};
  }

  // Fully visits the root object before any schema-specific parsing begins.
  Status ValidateRoot(simdjson::ondemand::object object) noexcept {
    return ValidateObject(object, {}, 1);
  }

 private:
  // Decodes every key and rejects duplicates within each object. Input views
  // remain in the parser's storage; this validator never retains them beyond
  // the complete traversal. Failed diagnostics are copied into error_.
  Status ValidateObject(simdjson::ondemand::object object,
                        std::string_view path, size_t depth) noexcept {
    simdjson::error_code json_error;
    const size_t object_id = next_object_id_++;
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
            InvalidConfig(child_path.view(), "duplicate object key", error_);
        error_.truncated |= child_path.truncated;
        return result;
      }
      const Status result =
          ValidateValue(field.value(), child_path.view(), depth + 1);
      if (!result.ok()) {
        error_.truncated |= child_path.truncated;
        return result;
      }
    }
    return {};
  }

  // Returns false only for a duplicate decoded key in the same object. The
  // allocation bound ensures the probe always encounters an empty table slot.
  bool InsertKey(size_t object_id, std::string_view key) noexcept {
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : key) {
      hash = (hash ^ byte) * 1099511628211ULL;
    }
    hash = (hash ^ object_id) * 1099511628211ULL;
    size_t slot = hash & (key_capacity_ - 1);
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

  // Fully decodes strings/containers. Numbers are lexically checked without
  // range conversion and consumed when the enclosing iterator advances.
  Status ValidateValue(simdjson::ondemand::value value, std::string_view path,
                       size_t depth) noexcept {
    JsonType type;
    simdjson::error_code json_error = value.type().get(type);
    if (json_error) {
      return JsonError(json_error, path, error_);
    }
    if ((type == JsonType::object || type == JsonType::array) &&
        depth > kMaxConfigDepth) {
      return InvalidConfig(path, "maximum JSON nesting depth is 64", error_);
    }
    switch (type) {
      case JsonType::object: {
        simdjson::ondemand::object object;
        json_error = value.get_object().get(object);
        if (json_error) {
          return JsonError(json_error, path, error_);
        }
        return ValidateObject(object, path, depth);
      }
      case JsonType::array: {
        simdjson::ondemand::array array;
        json_error = value.get_array().get(array);
        if (json_error) {
          return JsonError(json_error, path, error_);
        }
        size_t index = 0;
        for (auto value_result : array) {
          simdjson::ondemand::value child;
          json_error = std::move(value_result).get(child);
          if (json_error) {
            return JsonError(json_error, path, error_);
          }
          const ConfigPath child_path = IndexPath(path, index++);
          const Status result =
              ValidateValue(child, child_path.view(), depth + 1);
          if (!result.ok()) {
            error_.truncated |= child_path.truncated;
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
          return InvalidConfig(path, "invalid JSON number", error_);
        }
        // The enclosing iterator consumes this validated scalar on increment.
        return {};
      case JsonType::unknown:
        return InvalidConfig(path, "invalid JSON value", error_);
    }
    return json_error ? JsonError(json_error, path, error_) : Status{};
  }

  ConfigError& error_;
  KeyEntry* keys_ = nullptr;
  size_t key_capacity_ = 0;
  size_t next_object_id_ = 1;
};

// Validates the entire root object, including values that schemas may later
// reject. Numeric tokens retain JSON's unrestricted lexical number range.
Status Validate(const JsonConfigDocument& source, ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const Status parse_result =
      internal::ParseConfigObject(source, parser, document, root, error);
  if (!parse_result.ok()) {
    return parse_result;
  }
  JsonValidator validator(error);
  Status result = validator.Allocate(source.source().size());
  if (result.ok()) {
    result = validator.ValidateRoot(root);
  }
  if (!result.ok()) {
    return result;
  }
  if (!document.at_end()) {
    return InvalidConfig("$", "unexpected content after root object", error);
  }
  return {};
}

}  // namespace

JsonConfigDocument::~JsonConfigDocument() noexcept { Reset(); }

void JsonConfigDocument::Reset() noexcept {
  std::free(source_);
  source_ = nullptr;
  size_ = 0;
  capacity_ = 0;
}

Status JsonConfigDocument::Load(const char* path) noexcept {
  Reset();
  error_.Clear();
  if (path == nullptr || path[0] == '\0') {
    return InvalidConfig("$", "configuration file path must not be empty",
                         error_);
  }
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    const int saved_errno = errno;
    error_.Set("$", "cannot open configuration file");
    return {saved_errno == ENOMEM ? ErrorCode::kResourceExhausted
                                  : ErrorCode::kIoError,
            {saved_errno, std::generic_category()}};
  }
  // One extra byte detects oversized input; padding is required by simdjson.
  constexpr size_t kSourceCapacity =
      kMaxConfigFileBytes + 1 + simdjson::SIMDJSON_PADDING;
  source_ = static_cast<char*>(std::malloc(kSourceCapacity));
  if (source_ == nullptr) {
    (void)std::fclose(file);
    error_.Set("$", "cannot allocate configuration file buffer");
    return {ErrorCode::kResourceExhausted, {}};
  }
  const size_t size = std::fread(source_, 1, kMaxConfigFileBytes + 1, file);
  const bool read_error = std::ferror(file) != 0;
  const int read_errno = errno;
  const int close_result = std::fclose(file);
  const int close_errno = errno;
  if (read_error || close_result != 0) {
    Reset();
    error_.Set("$", "cannot read configuration file");
    return {ErrorCode::kIoError,
            {read_error ? read_errno : close_errno, std::generic_category()}};
  }
  if (size > kMaxConfigFileBytes) {
    Reset();
    return InvalidConfig("$", "configuration file exceeds the 1 MiB limit",
                         error_);
  }
  std::memset(source_ + size, 0, simdjson::SIMDJSON_PADDING);
  size_ = size;
  capacity_ = kSourceCapacity;
  const Status result = Validate(*this, error_);
  if (!result.ok()) {
    Reset();
  }
  return result;
}

}  // namespace yquant::base
