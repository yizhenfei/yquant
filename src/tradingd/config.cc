#include "tradingd/config.h"

#include <arpa/inet.h>
#include <simdjson.h>
#include <stddef.h>
#include <stdint.h>

#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "simdjson.h"
#include "yquant/base/json_config_document.h"
#include "yquant/base/path.h"

namespace yquant::tradingd {
namespace {

using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;
using base::internal::ReadConfigText;

base::Status Listen(simdjson::ondemand::value value,
                    trading::Endpoint& endpoint,
                    base::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) {
    return JsonError(code, "listen", error);
  }
  unsigned seen = 0;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) {
      return JsonError(code, "listen", error);
    }
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) {
      return JsonError(code, "listen", error);
    }
    if (key == "address") {
      const auto status = ReadConfigText(field.value(), endpoint.address,
                                         "listen.address", error);
      if (!status.ok()) {
        return status;
      }
      seen |= 1;
    } else if (key == "port") {
      uint64_t port = 0;
      code = field.value().get_uint64().get(port);
      if (code) {
        return JsonError(code, "listen.port", error);
      }
      if (port == 0 || port > UINT16_MAX) {
        return InvalidConfig("listen.port", "must be from 1 to 65535", error);
      }
      endpoint.port = static_cast<uint16_t>(port);
      seen |= 2;
    } else {
      return InvalidConfig("listen", "unknown field", error);
    }
  }
  if (seen != 3) {
    return InvalidConfig("listen", "address and port are required", error);
  }
  return {};
}

// Parses a bounded persistence block; duplicate keys are rejected by the
// configuration document before this pass.
base::Status Persistence(simdjson::ondemand::value value,
                         PersistenceConfig& config,
                         base::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) {
    return JsonError(code, "persistence", error);
  }
  unsigned seen = 0;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) {
      return JsonError(code, "persistence", error);
    }
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) {
      return JsonError(code, "persistence", error);
    }
    if (key == "directory" || key == "account_key") {
      const bool directory = key == "directory";
      auto status = ReadConfigText(
          field.value(),
          {directory ? config.directory : config.account_key,
           directory ? sizeof(config.directory) : sizeof(config.account_key)},
          directory ? "persistence.directory" : "persistence.account_key",
          error);
      if (!status.ok()) {
        return status;
      }
      seen |= directory ? 1 : 2;
      continue;
    }
    uint64_t number = 0;
    code = field.value().get_uint64().get(number);
    if (code) {
      return JsonError(code, "persistence", error);
    }
    if (key == "wal_segment_bytes") {
      config.wal_segment_bytes = number;
    } else if (key == "reserve_segments") {
      config.reserve_segments = number;
    } else if (key == "sync_interval_ms") {
      if (number > UINT32_MAX) {
        return InvalidConfig("persistence.sync_interval_ms", "too large",
                             error);
      }
      config.sync_interval_ms = static_cast<uint32_t>(number);
    } else if (key == "sync_after_bytes") {
      config.sync_after_bytes = number;
    } else if (key == "snapshot_interval_ms") {
      config.snapshot_interval_ms = number;
    } else {
      return InvalidConfig("persistence", "unknown field", error);
    }
  }
  if (seen != 3) {
    return InvalidConfig("persistence",
                         "directory and account_key are required", error);
  }
  return {};
}

// Copies a strict archive block without opening a connection or reading
// secrets.
base::Status MysqlArchive(simdjson::ondemand::value value,
                          MysqlArchiveOptions& config,
                          base::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) {
    return JsonError(code, "mysql_archive", error);
  }
  config.enabled = true;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) {
      return JsonError(code, "mysql_archive", error);
    }
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) {
      return JsonError(code, "mysql_archive", error);
    }
    if (key == "enabled") {
      code = field.value().get_bool().get(config.enabled);
      if (code) {
        return JsonError(code, "mysql_archive.enabled", error);
      }
      continue;
    }
    char* target = nullptr;
    size_t capacity = 0;
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
      auto status =
          ReadConfigText(field.value(), {target, capacity}, key, error);
      if (!status.ok()) {
        return status;
      }
      continue;
    }
    uint64_t number = 0;
    code = field.value().get_uint64().get(number);
    if (code) {
      return JsonError(code, "mysql_archive", error);
    }
    if (key == "min_free_bytes") {
      config.min_free_bytes = number;
      continue;
    }
    if (number > UINT32_MAX) {
      return InvalidConfig(key, "too large", error);
    }
    if (key == "port") {
      if (number == 0 || number > UINT16_MAX) {
        return InvalidConfig(key, "invalid port", error);
      }
      config.port = static_cast<uint16_t>(number);
    } else if (key == "timeout_seconds") {
      config.timeout_seconds = number;
    } else if (key == "batch_records") {
      config.batch_records = number;
    } else if (key == "batch_bytes") {
      config.batch_bytes = number;
    } else if (key == "poll_ms") {
      config.poll_ms = number;
    } else if (key == "retry_ms") {
      config.retry_ms = number;
    } else if (key == "shutdown_ms") {
      config.shutdown_ms = number;
    } else {
      return InvalidConfig("mysql_archive", "unknown field", error);
    }
  }
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
  if (!parse_status.ok()) {
    return parse_status;
  }
  simdjson::error_code code;
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
    if (code) {
      return JsonError(code, "$", error);
    }
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) {
      return JsonError(code, "$", error);
    }
    size_t index = 0;
    while (index < std::size(kFields) && key != kFields[index]) {
      ++index;
    }
    if (index == std::size(kFields)) {
      return InvalidConfig(key, "unknown field", error);
    }
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
      const auto capacity = md::kMaxRuntimeDirectoryLength + 1;
      status = ReadConfigText(field.value(),
                              {index == 3 ? config.token : target,
                               index == 3 ? sizeof(config.token) : capacity},
                              key, error);
    } else {
      uint64_t number = 0;
      code = field.value().get_uint64().get(number);
      if (code) {
        return JsonError(code, key, error);
      }
      if (index == 4) {
        if (number == 0 || number > trading::kMaxClients) {
          return InvalidConfig(key, "must be from 1 to 16", error);
        }
        config.max_clients = static_cast<size_t>(number);
      } else {
        if (number < kMinimumTimeoutMs || number > kMaximumTimeoutMs) {
          return InvalidConfig(key, "must be from 1000 to 60000 ms", error);
        }
        if (index == 5) {
          config.heartbeat_timeout_ns = number * kNanosecondsPerMillisecond;
        } else {
          config.shutdown_timeout_ns = number * kNanosecondsPerMillisecond;
        }
      }
    }
    if (!status.ok()) {
      return status;
    }
  }
  for (size_t i = 0; i < 4; ++i) {
    if ((seen & (1U << i)) == 0) {
      return InvalidConfig(kFields[i], "required field is missing", error);
    }
  }
  return {};
}

// Resolves a bounded configuration reference against cwd. Failure preserves
// value and sets the corresponding configuration field diagnostic.
base::Status Resolve(char* value, std::string_view field,
                     base::ConfigError& error) noexcept {
  const auto status =
      base::ResolvePath(value, {value, md::kMaxRuntimeDirectoryLength + 1});
  if (!status.ok()) {
    error.Set(field, status.code == base::ErrorCode::kIoError
                         ? "cannot resolve path from working directory"
                         : "resolved path is too long");
  }
  return status;
}

// Checks journal sizing, supported adapters and initialization requirements.
base::Status ValidatePersistence(const ServerConfig& config,
                                 base::ConfigError& error) noexcept {
  const auto& persistence = config.persistence;
  if (persistence.enabled()) {
    if (strnlen(persistence.directory, sizeof(persistence.directory)) >=
        sizeof(persistence.directory)) {
      return InvalidConfig("persistence.directory", "path too long", error);
    }
    const auto key_length =
        strnlen(persistence.account_key, sizeof(persistence.account_key));
    if (key_length == 0 || key_length > kMaxAccountKeyBytes) {
      return InvalidConfig("persistence.account_key",
                           "empty or overlong account key", error);
    }
    if (persistence.wal_segment_bytes < kMinWalSegmentBytes ||
        persistence.wal_segment_bytes > kJournalMaxSegmentBytes ||
        (persistence.wal_segment_bytes & (persistence.wal_segment_bytes - 1)) !=
            0) {
      return InvalidConfig("persistence.wal_segment_bytes",
                           "must be a power of two from 4 MiB to 1 GiB", error);
    }
    if (persistence.reserve_segments < kJournalMinReserveSegments ||
        persistence.reserve_segments > kJournalMaxReserveSegments) {
      return InvalidConfig("persistence.reserve_segments",
                           "must be from 2 to 16", error);
    }
    if (persistence.sync_interval_ms < kJournalMinSyncIntervalMs ||
        persistence.sync_interval_ms > kJournalMaxSyncIntervalMs) {
      return InvalidConfig("persistence.sync_interval_ms",
                           "must be from 1 to 1000", error);
    }
    if (persistence.sync_after_bytes < kJournalMinSyncAfterBytes ||
        persistence.sync_after_bytes > persistence.wal_segment_bytes) {
      return InvalidConfig("persistence.sync_after_bytes",
                           "must be from 4 KiB to segment capacity", error);
    }
    if (persistence.snapshot_interval_ms < kMinSnapshotIntervalMs ||
        persistence.snapshot_interval_ms > kMaxSnapshotIntervalMs) {
      return InvalidConfig("persistence.snapshot_interval_ms",
                           "must be from 1000 to 3600000", error);
    }
  } else if (persistence.initialize || persistence.account_key[0] != '\0') {
    return InvalidConfig("persistence.directory",
                         "required for persistent state", error);
  }
  return {};
}

// Checks archive connection and batch bounds against persistent-state settings.
base::Status ValidateArchive(const ServerConfig& config,
                             base::ConfigError& error) noexcept {
  const auto& persistence = config.persistence;
  const auto& archive = config.mysql_archive;
  if (archive.enabled) {
#ifndef YQUANT_WITH_MYSQL
    return InvalidConfig("mysql_archive", "build with YQUANT_WITH_MYSQL=ON",
                         error);
#endif
    if (!persistence.enabled()) {
      return InvalidConfig("mysql_archive", "requires persistence", error);
    }
    const auto bounded = [](const auto& text) noexcept {
      return text[0] != '\0' && strnlen(text, sizeof(text)) < sizeof(text);
    };
    if (!bounded(archive.host) || !bounded(archive.database) ||
        !bounded(archive.user) || !bounded(archive.password_env)) {
      return InvalidConfig(
          "mysql_archive",
          "host, database, user and password_env are required and bounded",
          error);
    }
    if (strnlen(archive.tls_ca, sizeof(archive.tls_ca)) >=
        sizeof(archive.tls_ca)) {
      return InvalidConfig("mysql_archive.tls_ca", "path too long", error);
    }
    const bool local = std::strcmp(archive.host, "127.0.0.1") == 0 ||
                       std::strcmp(archive.host, "localhost") == 0 ||
                       std::strcmp(archive.host, "::1") == 0;
    if (!local && archive.tls_ca[0] == '\0') {
      return InvalidConfig("mysql_archive.tls_ca", "required for remote MySQL",
                           error);
    }
    if (archive.port == 0 || archive.timeout_seconds == 0 ||
        archive.timeout_seconds > 30 || archive.batch_records == 0 ||
        archive.batch_records > 4096 ||
        archive.batch_bytes < kJournalMaxPayloadBytes ||
        archive.batch_bytes > 16 * 1024 * 1024 || archive.poll_ms == 0 ||
        archive.poll_ms > 60000 || archive.retry_ms == 0 ||
        archive.retry_ms > 60000 || archive.shutdown_ms > 60000) {
      return InvalidConfig("mysql_archive", "invalid timeout or batch bounds",
                           error);
    }
  }
  return {};
}

}  // namespace

base::Status ValidateConfig(const ServerConfig& config,
                            base::ConfigError& error) noexcept {
  error.Clear();
  in_addr address = {};
  if (strnlen(config.listen.address, sizeof(config.listen.address)) >=
          sizeof(config.listen.address) ||
      inet_pton(AF_INET, config.listen.address, &address) != 1) {
    return InvalidConfig("listen.address", "must be a numeric IPv4 address",
                         error);
  }
  const auto length = strnlen(config.token, sizeof(config.token));
  if (length == 0 || length > trading::kMaxTokenBytes) {
    return InvalidConfig("token", "must contain from 1 to 64 bytes", error);
  }
  if (config.max_clients == 0 || config.max_clients > trading::kMaxClients) {
    return InvalidConfig("max_clients", "must be from 1 to 16", error);
  }
  if (config.heartbeat_timeout_ns < kMinimumTimeoutNs ||
      config.heartbeat_timeout_ns > kMaximumTimeoutNs) {
    return InvalidConfig("heartbeat_timeout_ms",
                         "must be from 1000 to 60000 ms", error);
  }
  if (config.shutdown_timeout_ns < kMinimumTimeoutNs ||
      config.shutdown_timeout_ns > kMaximumTimeoutNs) {
    return InvalidConfig("shutdown_timeout_ms", "must be from 1000 to 60000 ms",
                         error);
  }
  if (config.persistence.enabled() &&
      config.trading.adapter != trading::TradingAdapterKind::kMock) {
    return InvalidConfig("persistence", "requires a cash mock adapter", error);
  }
  auto status = ValidatePersistence(config, error);
  if (status.ok()) {
    status = ValidateArchive(config, error);
  }
  if (!status.ok()) {
    return status;
  }
  status = md::ValidateMarketDataScope(config.market_data, error);
  if (!status.ok()) {
    return status;
  }
  status = trading::ValidateTradingConfig(config.trading, error);
  if (!status.ok()) {
    return status;
  }
  for (size_t i = 0; i < config.trading.initial_account.position_count; ++i) {
    const auto id = config.trading.initial_account.positions[i].instrument_id;
    bool found = false;
    for (size_t j = 0; j < config.market_data.instrument_count; ++j) {
      found |= config.market_data.instruments[j].instrument.id == id;
    }
    if (!found) {
      return InvalidConfig("trading_config.initial_positions",
                           "unknown instrument", error);
    }
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
  char market_path[md::kMaxRuntimeDirectoryLength + 1] = {};
  char trading_path[md::kMaxRuntimeDirectoryLength + 1] = {};
  status = Parse(document, parsed, market_path, trading_path, error_);
  if (!status.ok()) {
    return status;
  }
  status = Resolve(market_path, "market_data_config", error_);
  if (!status.ok()) {
    return status;
  }
  status = Resolve(trading_path, "trading_config", error_);
  if (!status.ok()) {
    return status;
  }
  md::MarketDataConfigLoader market_loader;
  status = market_loader.Load(market_path);
  if (!status.ok()) {
    error_ = market_loader.error();
    return status;
  }
  trading::TradingConfigLoader trading_loader;
  status = trading_loader.Load(trading_path);
  if (!status.ok()) {
    error_ = trading_loader.error();
    return status;
  }
  if (parsed.persistence.enabled()) {
    status =
        Resolve(parsed.persistence.directory, "persistence.directory", error_);
    if (!status.ok()) {
      return status;
    }
  }
  if (parsed.mysql_archive.tls_ca[0] != '\0') {
    status =
        Resolve(parsed.mysql_archive.tls_ca, "mysql_archive.tls_ca", error_);
    if (!status.ok()) {
      return status;
    }
  }
  parsed.market_data = market_loader.config();
  parsed.trading = trading_loader.config();
  status = ValidateConfig(parsed, error_);
  if (status.ok()) {
    config_ = parsed;
  }
  return status;
}

}  // namespace yquant::tradingd
