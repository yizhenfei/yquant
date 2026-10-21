#include "tradingd/config.h"

#include <arpa/inet.h>
#include <simdjson.h>

#include <cstring>
#include <string_view>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "yquant/base/json_config_document.h"
#include "yquant/base/path.h"

namespace yquant::tradingd {
namespace {

using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;
using base::internal::ReadConfigText;

base::Status Listen(simdjson::ondemand::value value, Endpoint& endpoint,
                    base::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, "listen", error);
  unsigned seen = 0;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, "listen", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "listen", error);
    if (key == "address") {
      const auto status = ReadConfigText(field.value(), endpoint.address,
                                         "listen.address", error);
      if (!status.ok()) return status;
      seen |= 1;
    } else if (key == "port") {
      std::uint64_t port = 0;
      code = field.value().get_uint64().get(port);
      if (code) return JsonError(code, "listen.port", error);
      if (port == 0 || port > UINT16_MAX)
        return InvalidConfig("listen.port", "must be from 1 to 65535", error);
      endpoint.port = static_cast<std::uint16_t>(port);
      seen |= 2;
    } else {
      return InvalidConfig("listen", "unknown field", error);
    }
  }
  if (seen != 3)
    return InvalidConfig("listen", "address and port are required", error);
  return {};
}

// Parses into temporary values; referenced file paths are resolved later.
base::Status Parse(const base::JsonConfigDocument& source, ServerConfig& config,
                   char* market_path, char* trading_path,
                   base::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const auto parse_status =
      ParseConfigObject(source, parser, document, root, error);
  if (!parse_status.ok()) return parse_status;
  simdjson::error_code code;
  constexpr std::string_view kFields[] = {
      "listen",      "market_data_config",   "trading_config",     "token",
      "max_clients", "heartbeat_timeout_ms", "shutdown_timeout_ms"};
  unsigned seen = 0;
  for (auto item : root) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, "$", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "$", error);
    std::size_t index = 0;
    while (index < std::size(kFields) && key != kFields[index]) ++index;
    if (index == std::size(kFields))
      return InvalidConfig(key, "unknown field", error);
    seen |= 1U << index;
    base::Status status;
    if (index == 0) {
      status = Listen(field.value(), config.listen, error);
    } else if (index < 4) {
      char* target = index == 1 ? market_path : trading_path;
      const auto capacity = strategyd::kMaxRuntimeDirectoryLength + 1;
      status = ReadConfigText(field.value(),
                              {index == 3 ? config.token : target,
                               index == 3 ? sizeof(config.token) : capacity},
                              key, error);
    } else {
      std::uint64_t number = 0;
      code = field.value().get_uint64().get(number);
      if (code) return JsonError(code, key, error);
      if (index == 4) {
        if (number == 0 || number > kMaxClients)
          return InvalidConfig(key, "must be from 1 to 16", error);
        config.max_clients = static_cast<std::size_t>(number);
      } else {
        if (number < kMinimumTimeoutMs || number > kMaximumTimeoutMs)
          return InvalidConfig(key, "must be from 1000 to 60000 ms", error);
        if (index == 5)
          config.heartbeat_timeout_ns = number * kNanosecondsPerMillisecond;
        else
          config.shutdown_timeout_ns = number * kNanosecondsPerMillisecond;
      }
    }
    if (!status.ok()) return status;
  }
  for (std::size_t i = 0; i < 4; ++i) {
    if ((seen & (1U << i)) == 0)
      return InvalidConfig(kFields[i], "required field is missing", error);
  }
  return {};
}

// Resolves a bounded configuration reference against cwd. Failure preserves
// value and sets the corresponding configuration field diagnostic.
base::Status Resolve(char* value, std::string_view field,
                     base::ConfigError& error) noexcept {
  const auto status = base::ResolvePath(
      value, {value, strategyd::kMaxRuntimeDirectoryLength + 1});
  if (!status.ok())
    error.Set(field, status.code == base::ErrorCode::kIoError
                         ? "cannot resolve path from working directory"
                         : "resolved path is too long");
  return status;
}

}  // namespace

base::Status ValidateConfig(const ServerConfig& config,
                            base::ConfigError& error) noexcept {
  error.Clear();
  in_addr address = {};
  if (strnlen(config.listen.address, sizeof(config.listen.address)) >=
          sizeof(config.listen.address) ||
      inet_pton(AF_INET, config.listen.address, &address) != 1)
    return InvalidConfig("listen.address", "must be a numeric IPv4 address",
                         error);
  const auto length = strnlen(config.token, sizeof(config.token));
  if (length == 0 || length > kMaxTokenBytes)
    return InvalidConfig("token", "must contain from 1 to 64 bytes", error);
  if (config.max_clients == 0 || config.max_clients > kMaxClients)
    return InvalidConfig("max_clients", "must be from 1 to 16", error);
  if (config.heartbeat_timeout_ns < kMinimumTimeoutNs ||
      config.heartbeat_timeout_ns > kMaximumTimeoutNs)
    return InvalidConfig("heartbeat_timeout_ms",
                         "must be from 1000 to 60000 ms", error);
  if (config.shutdown_timeout_ns < kMinimumTimeoutNs ||
      config.shutdown_timeout_ns > kMaximumTimeoutNs)
    return InvalidConfig("shutdown_timeout_ms", "must be from 1000 to 60000 ms",
                         error);
  auto status = strategyd::ValidateMarketDataScope(config.market_data, error);
  if (!status.ok()) return status;
  status = strategyd::ValidateTradingConfig(config.trading, error);
  if (!status.ok()) return status;
  if (config.trading.adapter != strategyd::TradingAdapterKind::kMock)
    return InvalidConfig("trading_config.adapter", "must be mock", error);
  for (std::size_t i = 0; i < config.trading.initial_account.position_count;
       ++i) {
    const auto id = config.trading.initial_account.positions[i].instrument_id;
    bool found = false;
    for (std::size_t j = 0; j < config.market_data.instrument_count; ++j)
      found |= config.market_data.instruments[j].instrument.id == id;
    if (!found)
      return InvalidConfig("trading_config.initial_positions",
                           "unknown instrument", error);
  }
  return {};
}

base::Status ConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  base::JsonConfigDocument document;
  auto status = document.Load(path);
  if (!status.ok()) {
    error_ = document.error();
    return status;
  }
  ServerConfig parsed;
  char market_path[strategyd::kMaxRuntimeDirectoryLength + 1] = {};
  char trading_path[strategyd::kMaxRuntimeDirectoryLength + 1] = {};
  status = Parse(document, parsed, market_path, trading_path, error_);
  if (!status.ok()) return status;
  status = Resolve(market_path, "market_data_config", error_);
  if (!status.ok()) return status;
  status = Resolve(trading_path, "trading_config", error_);
  if (!status.ok()) return status;
  strategyd::MarketDataConfigLoader market_loader;
  status = market_loader.Load(market_path);
  if (!status.ok()) {
    error_ = market_loader.error();
    return status;
  }
  strategyd::TradingConfigLoader trading_loader;
  status = trading_loader.Load(trading_path);
  if (!status.ok()) {
    error_ = trading_loader.error();
    return status;
  }
  parsed.market_data = market_loader.config();
  parsed.trading = trading_loader.config();
  status = ValidateConfig(parsed, error_);
  if (status.ok()) config_ = parsed;
  return status;
}

}  // namespace yquant::tradingd
