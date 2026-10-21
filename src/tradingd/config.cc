#include "tradingd/config.h"

#include <arpa/inet.h>
#include <simdjson.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <utility>

#include "strategyd/json_config_document.h"

namespace yquant::tradingd {
namespace {

base::Status Invalid(std::string_view field, std::string_view reason,
                     strategy::ConfigError& error) noexcept {
  error.Set(field, reason);
  return {base::ErrorCode::kInvalidArgument, {}};
}

base::Status JsonError(simdjson::error_code code, std::string_view field,
                       strategy::ConfigError& error) noexcept {
  error.Set(field, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Copies owned text, rejecting truncation and embedded NUL.
base::Status Text(simdjson::ondemand::value value, char* out,
                  std::size_t capacity, std::string_view path,
                  strategy::ConfigError& error) noexcept {
  std::string_view text;
  const auto code = value.get_string().get(text);
  if (code) return JsonError(code, path, error);
  if (text.empty() || text.size() >= capacity ||
      text.find('\0') != std::string_view::npos)
    return Invalid(path, "empty, overlong or NUL-containing string", error);
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return {};
}

base::Status Listen(simdjson::ondemand::value value, Endpoint& endpoint,
                    strategy::ConfigError& error) noexcept {
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
      const auto status =
          Text(field.value(), endpoint.address, sizeof(endpoint.address),
               "listen.address", error);
      if (!status.ok()) return status;
      seen |= 1;
    } else if (key == "port") {
      std::uint64_t port = 0;
      code = field.value().get_uint64().get(port);
      if (code) return JsonError(code, "listen.port", error);
      if (port == 0 || port > UINT16_MAX)
        return Invalid("listen.port", "must be from 1 to 65535", error);
      endpoint.port = static_cast<std::uint16_t>(port);
      seen |= 2;
    } else {
      return Invalid("listen", "unknown field", error);
    }
  }
  if (seen != 3)
    return Invalid("listen", "address and port are required", error);
  return {};
}

// Parses a bounded persistence block; duplicate keys are rejected by the
// configuration document before this pass.
base::Status Persistence(simdjson::ondemand::value value,
                         PersistenceConfig& config,
                         strategy::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, "persistence", error);
  unsigned seen = 0;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, "persistence", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "persistence", error);
    if (key == "directory" || key == "account_key") {
      const bool directory = key == "directory";
      auto status = Text(
          field.value(), directory ? config.directory : config.account_key,
          directory ? sizeof(config.directory) : sizeof(config.account_key),
          directory ? "persistence.directory" : "persistence.account_key",
          error);
      if (!status.ok()) return status;
      seen |= directory ? 1 : 2;
      continue;
    }
    std::uint64_t number = 0;
    code = field.value().get_uint64().get(number);
    if (code) return JsonError(code, "persistence", error);
    if (key == "wal_segment_bytes")
      config.wal_segment_bytes = number;
    else if (key == "reserve_segments")
      config.reserve_segments = number;
    else if (key == "sync_interval_ms") {
      if (number > UINT32_MAX)
        return Invalid("persistence.sync_interval_ms", "too large", error);
      config.sync_interval_ms = static_cast<std::uint32_t>(number);
    } else if (key == "sync_after_bytes")
      config.sync_after_bytes = number;
    else if (key == "snapshot_interval_ms")
      config.snapshot_interval_ms = number;
    else
      return Invalid("persistence", "unknown field", error);
  }
  if (seen != 3)
    return Invalid("persistence", "directory and account_key are required",
                   error);
  return {};
}

// Copies a strict archive block without opening a connection or reading
// secrets.
base::Status MysqlArchive(simdjson::ondemand::value value,
                          MysqlArchiveOptions& config,
                          strategy::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, "mysql_archive", error);
  config.enabled = true;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, "mysql_archive", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "mysql_archive", error);
    if (key == "enabled") {
      code = field.value().get_bool().get(config.enabled);
      if (code) return JsonError(code, "mysql_archive.enabled", error);
      continue;
    }
    char* target = nullptr;
    std::size_t capacity = 0;
    if (key == "host") {
      target = config.host;
      capacity = sizeof(config.host);
    } else if (key == "database") {
      target = config.database;
      capacity = sizeof(config.database);
    } else if (key == "user") {
      target = config.user;
      capacity = sizeof(config.user);
    } else if (key == "password_env") {
      target = config.password_env;
      capacity = sizeof(config.password_env);
    } else if (key == "tls_ca") {
      target = config.tls_ca;
      capacity = sizeof(config.tls_ca);
    }
    if (target != nullptr) {
      auto status = Text(field.value(), target, capacity, key, error);
      if (!status.ok()) return status;
      continue;
    }
    std::uint64_t number = 0;
    code = field.value().get_uint64().get(number);
    if (code) return JsonError(code, "mysql_archive", error);
    if (key == "min_free_bytes") {
      config.min_free_bytes = number;
      continue;
    }
    if (number > UINT32_MAX) return Invalid(key, "too large", error);
    if (key == "port") {
      if (number == 0 || number > UINT16_MAX)
        return Invalid(key, "invalid port", error);
      config.port = static_cast<std::uint16_t>(number);
    } else if (key == "timeout_seconds")
      config.timeout_seconds = number;
    else if (key == "batch_records")
      config.batch_records = number;
    else if (key == "batch_bytes")
      config.batch_bytes = number;
    else if (key == "poll_ms")
      config.poll_ms = number;
    else if (key == "retry_ms")
      config.retry_ms = number;
    else if (key == "shutdown_ms")
      config.shutdown_ms = number;
    else
      return Invalid("mysql_archive", "unknown field", error);
  }
  return {};
}

// Parses into temporary values; referenced file paths are resolved later.
base::Status Parse(const strategyd::JsonConfigDocument& source,
                   ServerConfig& config, char* market_path, char* trading_path,
                   strategy::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  auto code =
      parser.allocate(source.source().size(), strategyd::kMaxConfigDepth + 4);
  if (code) return JsonError(code, "$", error);
  simdjson::ondemand::document document;
  code = parser.iterate(source.source(), source.capacity()).get(document);
  if (code) return JsonError(code, "$", error);
  simdjson::ondemand::object root;
  code = document.get_object().get(root);
  if (code) return JsonError(code, "$", error);
  constexpr std::string_view kFields[] = {"listen",
                                          "market_data_config",
                                          "trading_config",
                                          "token",
                                          "max_clients",
                                          "heartbeat_timeout_ms",
                                          "shutdown_timeout_ms",
                                          "persistence",
                                          "mysql_archive"};
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
      return Invalid(key, "unknown field", error);
    seen |= 1U << index;
    base::Status status;
    if (index == 8) {
      status = MysqlArchive(field.value(), config.mysql_archive, error);
    } else if (index == 7) {
      status = Persistence(field.value(), config.persistence, error);
    } else if (index == 0) {
      status = Listen(field.value(), config.listen, error);
    } else if (index < 4) {
      char* target = index == 1 ? market_path : trading_path;
      const auto capacity = strategyd::kMaxRuntimeDirectoryLength + 1;
      status = Text(field.value(), index == 3 ? config.token : target,
                    index == 3 ? sizeof(config.token) : capacity, key, error);
    } else {
      std::uint64_t number = 0;
      code = field.value().get_uint64().get(number);
      if (code) return JsonError(code, key, error);
      if (index == 4) {
        if (number == 0 || number > kMaxClients)
          return Invalid(key, "must be from 1 to 16", error);
        config.max_clients = static_cast<std::size_t>(number);
      } else {
        if (number < kMinimumTimeoutMs || number > kMaximumTimeoutMs)
          return Invalid(key, "must be from 1000 to 60000 ms", error);
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
      return Invalid(kFields[i], "required field is missing", error);
  }
  return {};
}

// Resolves a bounded referenced path using the parent configuration directory.
base::Status Resolve(char* value, const char* parent, std::string_view field,
                     strategy::ConfigError& error) noexcept {
  auto path = std::filesystem::path(value);
  if (path.is_relative())
    path = std::filesystem::path(parent).parent_path() / path;
  if (path.native().size() > strategyd::kMaxRuntimeDirectoryLength)
    return Invalid(field, "resolved path is too long", error);
  std::snprintf(value, strategyd::kMaxRuntimeDirectoryLength + 1, "%s",
                path.c_str());
  return {};
}

}  // namespace

base::Status ValidateConfig(const ServerConfig& config,
                            strategy::ConfigError& error) noexcept {
  error.Clear();
  in_addr address = {};
  if (strnlen(config.listen.address, sizeof(config.listen.address)) >=
          sizeof(config.listen.address) ||
      inet_pton(AF_INET, config.listen.address, &address) != 1)
    return Invalid("listen.address", "must be a numeric IPv4 address", error);
  const auto length = strnlen(config.token, sizeof(config.token));
  if (length == 0 || length > kMaxTokenBytes)
    return Invalid("token", "must contain from 1 to 64 bytes", error);
  if (config.max_clients == 0 || config.max_clients > kMaxClients)
    return Invalid("max_clients", "must be from 1 to 16", error);
  if (config.heartbeat_timeout_ns < kMinimumTimeoutNs ||
      config.heartbeat_timeout_ns > kMaximumTimeoutNs)
    return Invalid("heartbeat_timeout_ms", "must be from 1000 to 60000 ms",
                   error);
  if (config.shutdown_timeout_ns < kMinimumTimeoutNs ||
      config.shutdown_timeout_ns > kMaximumTimeoutNs)
    return Invalid("shutdown_timeout_ms", "must be from 1000 to 60000 ms",
                   error);
  const auto& persistence = config.persistence;
  if (persistence.enabled()) {
    if (strnlen(persistence.directory, sizeof(persistence.directory)) >=
        sizeof(persistence.directory))
      return Invalid("persistence.directory", "path too long", error);
    const auto key_length =
        strnlen(persistence.account_key, sizeof(persistence.account_key));
    if (key_length == 0 || key_length > kMaxAccountKeyBytes)
      return Invalid("persistence.account_key", "empty or overlong account key",
                     error);
    if (persistence.wal_segment_bytes < kMinWalSegmentBytes ||
        persistence.wal_segment_bytes > kJournalMaxSegmentBytes ||
        (persistence.wal_segment_bytes & (persistence.wal_segment_bytes - 1)) !=
            0)
      return Invalid("persistence.wal_segment_bytes",
                     "must be a power of two from 4 MiB to 1 GiB", error);
    if (persistence.reserve_segments < kJournalMinReserveSegments ||
        persistence.reserve_segments > kJournalMaxReserveSegments)
      return Invalid("persistence.reserve_segments", "must be from 2 to 16",
                     error);
    if (persistence.sync_interval_ms < kJournalMinSyncIntervalMs ||
        persistence.sync_interval_ms > kJournalMaxSyncIntervalMs)
      return Invalid("persistence.sync_interval_ms", "must be from 1 to 1000",
                     error);
    if (persistence.sync_after_bytes < kJournalMinSyncAfterBytes ||
        persistence.sync_after_bytes > persistence.wal_segment_bytes)
      return Invalid("persistence.sync_after_bytes",
                     "must be from 4 KiB to segment capacity", error);
    if (persistence.snapshot_interval_ms < kMinSnapshotIntervalMs ||
        persistence.snapshot_interval_ms > kMaxSnapshotIntervalMs)
      return Invalid("persistence.snapshot_interval_ms",
                     "must be from 1000 to 3600000", error);
  } else if (persistence.initialize || persistence.account_key[0] != '\0') {
    return Invalid("persistence.directory", "required for persistent state",
                   error);
  }
  const auto& archive = config.mysql_archive;
  if (archive.enabled) {
#ifndef YQUANT_WITH_MYSQL
    return Invalid("mysql_archive", "build with YQUANT_WITH_MYSQL=ON", error);
#endif
    if (!persistence.enabled())
      return Invalid("mysql_archive", "requires persistence", error);
    const auto bounded = [](const auto& text) noexcept {
      return text[0] != '\0' && strnlen(text, sizeof(text)) < sizeof(text);
    };
    if (!bounded(archive.host) || !bounded(archive.database) ||
        !bounded(archive.user) || !bounded(archive.password_env))
      return Invalid(
          "mysql_archive",
          "host, database, user and password_env are required and bounded",
          error);
    if (strnlen(archive.tls_ca, sizeof(archive.tls_ca)) >=
        sizeof(archive.tls_ca))
      return Invalid("mysql_archive.tls_ca", "path too long", error);
    const bool local = std::strcmp(archive.host, "127.0.0.1") == 0 ||
                       std::strcmp(archive.host, "localhost") == 0 ||
                       std::strcmp(archive.host, "::1") == 0;
    if (!local && archive.tls_ca[0] == '\0')
      return Invalid("mysql_archive.tls_ca", "required for remote MySQL",
                     error);
    if (archive.port == 0 || archive.timeout_seconds == 0 ||
        archive.timeout_seconds > 30 || archive.batch_records == 0 ||
        archive.batch_records > 4096 ||
        archive.batch_bytes < kJournalMaxPayloadBytes ||
        archive.batch_bytes > 16 * 1024 * 1024 || archive.poll_ms == 0 ||
        archive.poll_ms > 60000 || archive.retry_ms == 0 ||
        archive.retry_ms > 60000 || archive.shutdown_ms > 60000)
      return Invalid("mysql_archive", "invalid timeout or batch bounds", error);
  }
  auto status = strategyd::ValidateMarketDataConfig(config.market_data, error);
  if (!status.ok()) return status;
  status = strategyd::ValidateTradingConfig(config.trading, error);
  if (!status.ok()) return status;
  if (config.trading.adapter != strategyd::TradingAdapterKind::kMock)
    return Invalid("trading_config.adapter", "must be mock", error);
  for (std::size_t i = 0; i < config.trading.initial_account.position_count;
       ++i) {
    const auto id = config.trading.initial_account.positions[i].instrument_id;
    bool found = false;
    for (std::size_t j = 0; j < config.market_data.instrument_count; ++j)
      found |= config.market_data.instruments[j].instrument.id == id;
    if (!found)
      return Invalid("trading_config.initial_positions", "unknown instrument",
                     error);
  }
  return {};
}

base::Status ConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  strategyd::JsonConfigDocument document;
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
  status = Resolve(market_path, path, "market_data_config", error_);
  if (!status.ok()) return status;
  status = Resolve(trading_path, path, "trading_config", error_);
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
  if (parsed.persistence.enabled()) {
    status = Resolve(parsed.persistence.directory, path,
                     "persistence.directory", error_);
    if (!status.ok()) return status;
  }
  if (parsed.mysql_archive.tls_ca[0] != '\0') {
    status = Resolve(parsed.mysql_archive.tls_ca, path, "mysql_archive.tls_ca",
                     error_);
    if (!status.ok()) return status;
  }
  parsed.market_data = market_loader.config();
  parsed.trading = trading_loader.config();
  status = ValidateConfig(parsed, error_);
  if (status.ok()) config_ = parsed;
  return status;
}

}  // namespace yquant::tradingd
