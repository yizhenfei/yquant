#include "yquant/trading/trading_client_config.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstring>
#include <string_view>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "simdjson.h"
#include "yquant/base/json_config_document.h"

namespace yquant::trading {
namespace {
using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;

// Copies decoded strings into their owned protocol fields. Failure preserves
// the destination; document/parser storage never escapes the loader.
template <size_t kSize>
base::Status CopyString(std::string_view value, char (&out)[kSize],
                        std::string_view field,
                        base::ConfigError& error) noexcept {
  if (value.empty() || value.size() >= kSize ||
      value.find('\0') != value.npos) {
    return InvalidConfig(field, "string is empty or exceeds field capacity",
                         error);
  }
  std::memcpy(out, value.data(), value.size());
  out[value.size()] = '\0';
  return {};
}

// Parses one validated JSON object into temporary owned client values.
base::Status Parse(const base::JsonConfigDocument& source,
                   TradingClientConfig& config,
                   base::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const auto parsed = ParseConfigObject(source, parser, document, root, error);
  if (!parsed.ok()) {
    return parsed;
  }
  constexpr std::array<std::string_view, 8> kFields = {"adapter",
                                                       "address",
                                                       "port",
                                                       "client_id",
                                                       "token",
                                                       "connect_timeout_ms",
                                                       "heartbeat_timeout_ms",
                                                       "shutdown_timeout_ms"};
  constexpr uint32_t kRequiredFields = (1U << 5) - 1;
  uint32_t seen = 0;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    auto json_error = std::move(field_result).get(field);
    if (json_error) {
      return JsonError(json_error, "$", error);
    }
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) {
      return JsonError(json_error, "$", error);
    }
    size_t index = 0;
    while (index < kFields.size() && kFields[index] != key) {
      ++index;
    }
    if (index == kFields.size()) {
      return InvalidConfig(key, "unknown client field", error);
    }
    seen |= 1U << index;
    if (index == 0 || index == 1 || index == 4) {
      std::string_view value;
      json_error = field.value().get_string().get(value);
      if (json_error) {
        return JsonError(json_error, key, error);
      }
      base::Status status;
      if (index == 0) {
        if (value != "tcp") {
          return InvalidConfig(key, "must be tcp; trading requires tradingd",
                               error);
        }
      } else if (index == 1) {
        status = CopyString(value, config.endpoint.address, key, error);
      } else {
        status = CopyString(value, config.token, key, error);
      }
      if (!status.ok()) {
        return status;
      }
    } else {
      uint64_t value;
      json_error = field.value().get_uint64().get(value);
      if (json_error) {
        return JsonError(json_error, key, error);
      }
      if (index == 2) {
        if (value == 0 || value > UINT16_MAX) {
          return InvalidConfig(key, "must be from 1 to 65535", error);
        }
        config.endpoint.port = static_cast<uint16_t>(value);
      } else if (index == 3) {
        config.client_id = value;
      } else {
        if (value < kMinimumTradingDurationMs ||
            value > kMaximumTradingDurationMs) {
          return InvalidConfig(key, "must be an integer from 1 to 60000",
                               error);
        }
        const auto duration = value * kTradingNanosecondsPerMillisecond;
        if (index == 5) {
          config.connect_timeout_ns = duration;
        } else if (index == 6) {
          config.heartbeat_timeout_ns = duration;
        } else {
          config.shutdown_timeout_ns = duration;
        }
      }
    }
  }
  for (size_t index = 0; index < kFields.size(); ++index) {
    if ((kRequiredFields & (1U << index)) && !(seen & (1U << index))) {
      return InvalidConfig(kFields[index], "required field is missing", error);
    }
  }
  return {};
}
}  // namespace

base::Status ValidateTradingClientConfig(const TradingClientConfig& config,
                                         base::ConfigError& error) noexcept {
  error.Clear();
  in_addr address;
  if (!std::memchr(config.endpoint.address, '\0',
                   sizeof(config.endpoint.address)) ||
      ::inet_pton(AF_INET, config.endpoint.address, &address) != 1 ||
      address.s_addr == INADDR_ANY) {
    return InvalidConfig("address", "requires a nonzero numeric IPv4 address",
                         error);
  }
  if (config.endpoint.port == 0) {
    return InvalidConfig("port", "must be from 1 to 65535", error);
  }
  if (config.client_id == 0) {
    return InvalidConfig("client_id", "must be nonzero", error);
  }
  const auto* end = static_cast<const char*>(
      std::memchr(config.token, '\0', sizeof(config.token)));
  if (!end || end == config.token) {
    return InvalidConfig("token", "requires 1 to 64 printable ASCII bytes",
                         error);
  }
  for (const char* byte = config.token; byte != end; ++byte) {
    if (*byte < '!' || *byte > '~') {
      return InvalidConfig("token", "requires 1 to 64 printable ASCII bytes",
                           error);
    }
  }
  constexpr uint64_t kMinimum =
      kMinimumTradingDurationMs * kTradingNanosecondsPerMillisecond;
  constexpr uint64_t kMaximum =
      kMaximumTradingDurationMs * kTradingNanosecondsPerMillisecond;
  struct Timeout {
    std::string_view field;
    uint64_t value;
  };
  const std::array<Timeout, 3> timeouts = {
      {{"connect_timeout_ms", config.connect_timeout_ns},
       {"heartbeat_timeout_ms", config.heartbeat_timeout_ns},
       {"shutdown_timeout_ms", config.shutdown_timeout_ns}}};
  for (const auto& [field, value] : timeouts) {
    if (value < kMinimum || value > kMaximum) {
      return InvalidConfig(field, "must be from 1 to 60000 milliseconds",
                           error);
    }
  }
  return {};
}

base::Status TradingClientConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  base::JsonConfigDocument source;
  auto status = source.Load(path);
  if (!status.ok()) {
    error_ = source.error();
    return status;
  }
  TradingClientConfig parsed;
  status = Parse(source, parsed, error_);
  if (status.ok()) {
    status = ValidateTradingClientConfig(parsed, error_);
  }
  if (status.ok()) {
    config_ = parsed;
  }
  return status;
}
}  // namespace yquant::trading
