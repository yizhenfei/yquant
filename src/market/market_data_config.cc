#include "yquant/market/market_data_config.h"

#include <stddef.h>
#include <stdint.h>

#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "simdjson.h"
#include "yquant/base/format.h"
#include "yquant/base/json_config_document.h"
#include "yquant/base/path.h"
#include "yquant/market/instrument_catalog.h"

namespace yquant::market {
namespace {

using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;
using base::internal::ReadConfigText;

// Configuration reference/resolved-path limit, excluding the terminating NUL.
constexpr size_t kMaxInstrumentCatalogPathLength = 4095;

// Bounded diagnostic path for a field in the instrument array.
struct FieldPath {
  char text[sizeof(base::ConfigError::field_path)] = {};
  bool truncated = false;

  FieldPath(size_t index, std::string_view key) noexcept {
    const int count = ::yquant::base::FormatTo(
        text, sizeof(text), "instruments[{}].{}", index,
        ::yquant::base::CStringPrefix(key.data(),
                                      static_cast<int>(key.size())));
    truncated = count < 0 || static_cast<size_t>(count) >= sizeof(text);
  }
};

// Reads unsigned integers without any intermediate floating-point conversion.
base::Status ReadUnsigned(simdjson::ondemand::value value,
                          std::string_view path, uint64_t& out,
                          base::ConfigError& error) noexcept {
  const auto code = value.get_uint64().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

// Borrows the decoded string only while the schema parser is alive.
base::Status ReadText(simdjson::ondemand::value value, std::string_view path,
                      std::string_view& out,
                      base::ConfigError& error) noexcept {
  const auto code = value.get_string().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

// Maps only supported venue codes; empty denotes invalid typed definitions.

// Loads an instrument identity into temporary receiver storage. Metadata is
// resolved from the shared catalog; no JSON views survive parsing.
base::Status ParseInstrument(simdjson::ondemand::value value, size_t index,
                             MarketDataInstrumentConfig& out,
                             base::ConfigError& error) noexcept {
  const FieldPath object_path(index, "");
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) {
    return JsonError(json_error, object_path.text, error);
  }
  MarketDataInstrumentConfig parsed;
  std::string_view venue;
  std::string_view symbol;
  uint32_t seen = 0;
  constexpr std::string_view kFields[] = {"venue", "symbol"};
  for (auto field_result : object) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) {
      return JsonError(json_error, object_path.text, error);
    }
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) {
      return JsonError(json_error, object_path.text, error);
    }
    const FieldPath path(index, key);
    size_t position = 0;
    while (position < std::size(kFields) && kFields[position] != key) {
      ++position;
    }
    if (position == std::size(kFields)) {
      const auto result =
          InvalidConfig(path.text, "unknown instrument field", error);
      error.truncated |= path.truncated;
      return result;
    }
    seen |= 1U << position;
    base::Status result;
    switch (position) {
      case 0:
        result = ReadText(field.value(), path.text, venue, error);
        break;
      case 1:
        result = ReadText(field.value(), path.text, symbol, error);
        break;
    }
    if (!result.ok()) {
      return result;
    }
  }
  for (size_t field = 0; field < 2; ++field) {
    if ((seen & (1U << field)) == 0) {
      return InvalidConfig(FieldPath(index, kFields[field]).text,
                           "required field is missing", error);
    }
  }
  parsed.instrument.venue = model::ParseVenue(venue);
  if (parsed.instrument.venue == model::Venue::kUnknown) {
    return InvalidConfig(FieldPath(index, "venue").text, "unsupported venue",
                         error);
  }
  const auto id_result =
      model::MakeInstrumentId(venue, symbol, parsed.instrument.id);
  if (id_result != base::ErrorCode::kOk) {
    return InvalidConfig(FieldPath(index, "symbol").text,
                         "must contain exactly six ASCII digits", error);
  }
  std::memcpy(parsed.instrument.symbol, symbol.data(), symbol.size());
  out = parsed;
  return {};
}

// Reads the bounded instrument array; the generic document already validated
// all nested values and decoded duplicate keys, including rejected entries.
base::Status ParseInstruments(simdjson::ondemand::value value,
                              MarketDataConfig& out,
                              base::ConfigError& error) noexcept {
  simdjson::ondemand::array array;
  auto json_error = value.get_array().get(array);
  if (json_error) {
    return JsonError(json_error, "instruments", error);
  }
  for (auto result : array) {
    if (out.instrument_count == kMaxMarketDataInstruments) {
      return InvalidConfig("instruments", "at most 64 instruments are allowed",
                           error);
    }
    simdjson::ondemand::value item;
    json_error = std::move(result).get(item);
    if (json_error) {
      return JsonError(json_error, "instruments", error);
    }
    const auto status =
        ParseInstrument(item, out.instrument_count,
                        out.instruments[out.instrument_count], error);
    if (!status.ok()) {
      return status;
    }
    ++out.instrument_count;
  }
  return {};
}

// Applies one adapter's schema after parsing all fields, allowing arbitrary
// JSON field order while rejecting fields belonging to the other adapter.
base::Status Parse(
    const base::JsonConfigDocument& source, MarketDataConfig& out,
    char (&catalog_reference)[kMaxInstrumentCatalogPathLength + 1],
    base::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const auto parsed = ParseConfigObject(source, parser, document, root, error);
  if (!parsed.ok()) {
    return parsed;
  }
  simdjson::error_code json_error;
  constexpr std::string_view kFields[] = {
      "source_id",          "adapter",     "instruments",
      "runtime_directory",  "stream",      "poll_interval_us",
      "instrument_catalog", "replay_path", "replay_speed",
      "replay_tail_ms"};
  uint32_t seen = 0;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) {
      return JsonError(json_error, "$", error);
    }
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) {
      return JsonError(json_error, "$", error);
    }
    size_t position = 0;
    while (position < std::size(kFields) && kFields[position] != key) {
      ++position;
    }
    if (position == std::size(kFields)) {
      return InvalidConfig(key, "unknown root field", error);
    }
    seen |= 1U << position;
    base::Status result;
    switch (position) {
      case 0: {
        uint64_t source_id;
        result = ReadUnsigned(field.value(), key, source_id, error);
        if (result.ok() && (source_id == 0 || source_id > UINT32_MAX)) {
          result = InvalidConfig(key, "must be an integer from 1 to UINT32_MAX",
                                 error);
        }
        if (result.ok()) {
          out.source_id.value = source_id;
        }
        break;
      }
      case 1: {
        std::string_view adapter;
        result = ReadText(field.value(), key, adapter, error);
        if (result.ok()) {
          if (adapter == "shm") {
            out.adapter = MarketDataAdapter::kShm;
          } else if (adapter == "replay") {
            out.adapter = MarketDataAdapter::kReplay;
          } else {
            result = InvalidConfig(key, "must be shm or replay", error);
          }
        }
        break;
      }
      case 2:
        result = ParseInstruments(field.value(), out, error);
        break;
      case 3:
        result =
            ReadConfigText(field.value(), out.runtime_directory, key, error,
                           "must be nonempty, bounded text without NUL");
        break;
      case 4:
        result = ReadConfigText(field.value(), out.stream, key, error,
                                "must be nonempty, bounded text without NUL");
        break;
      case 5: {
        uint64_t microseconds;
        result = ReadUnsigned(field.value(), key, microseconds, error);
        if (result.ok() && microseconds > 60000000) {
          result = InvalidConfig(key, "must be from 0 to 60000000", error);
        }
        if (result.ok()) {
          out.poll_interval_ns = microseconds * 1000;
        }
        break;
      }
      case 6:
        result = ReadConfigText(field.value(), catalog_reference, key, error,
                                "must be nonempty, bounded text without NUL");
        break;
      case 7:
        result = ReadConfigText(field.value(), out.replay_path, key, error);
        break;
      case 8: {
        uint64_t value = 0;
        result = ReadUnsigned(field.value(), key, value, error);
        if (result.ok() && (value == 0 || value > 1000)) {
          result = InvalidConfig(key, "must be from 1 to 1000", error);
        }
        if (result.ok()) {
          out.replay_speed = value;
        }
        break;
      }
      case 9: {
        uint64_t value = 0;
        result = ReadUnsigned(field.value(), key, value, error);
        if (result.ok() && value > 60000) {
          result = InvalidConfig(key, "must be from 0 to 60000", error);
        }
        if (result.ok()) {
          out.replay_tail_ns = value * 1000000;
        }
        break;
      }
    }
    if (!result.ok()) {
      return result;
    }
  }
  constexpr uint32_t kCommon = (1U << 0) | (1U << 1) | (1U << 2) | (1U << 6);
  const bool replay = out.adapter == MarketDataAdapter::kReplay;
  const auto required = kCommon | (replay ? (1U << 7) : (1U << 3) | (1U << 4));
  const auto allowed = required | (replay ? (1U << 8) | (1U << 9) : (1U << 5));
  for (size_t field = 0; field < std::size(kFields); ++field) {
    if ((required & (1U << field)) && !(seen & (1U << field))) {
      return InvalidConfig(kFields[field], "required field is missing", error);
    }
    if (!(allowed & (1U << field)) && (seen & (1U << field))) {
      return InvalidConfig(kFields[field],
                           "field is not valid for this adapter", error);
    }
  }
  return {};
}

// Verifies endpoint text without touching the filesystem or allocating.
base::Status ValidateShmEndpoint(const MarketDataConfig& config,
                                 base::ConfigError& error) noexcept {
  const auto* path_end = static_cast<const char*>(std::memchr(
      config.runtime_directory, '\0', sizeof(config.runtime_directory)));
  if (path_end == nullptr || config.runtime_directory[0] != '/') {
    return InvalidConfig("runtime_directory",
                         "must be an absolute bounded path", error);
  }
  const std::string_view path(config.runtime_directory,
                              path_end - config.runtime_directory);
  size_t offset = 0;
  while (offset <= path.size()) {
    const auto end = path.find('/', offset);
    const auto part = path.substr(
        offset, end == path.npos ? path.size() - offset : end - offset);
    if (part == "..") {
      return InvalidConfig("runtime_directory",
                           "parent traversal is not allowed", error);
    }
    if (end == path.npos) {
      break;
    }
    offset = end + 1;
  }
  const auto* stream_end = static_cast<const char*>(
      std::memchr(config.stream, '\0', sizeof(config.stream)));
  if (stream_end == nullptr || stream_end == config.stream) {
    return InvalidConfig("stream", "must contain from 1 to 64 ASCII characters",
                         error);
  }
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(config.stream[0])) {
    return InvalidConfig("stream", "must start with an ASCII letter or digit",
                         error);
  }
  for (const char* p = config.stream; p != stream_end; ++p) {
    if (!alnum(*p) && *p != '_' && *p != '-' && *p != '.') {
      return InvalidConfig("stream", "contains an invalid character", error);
    }
  }
  if (config.poll_interval_ns > 60000000000ULL) {
    return InvalidConfig("poll_interval_us", "must be from 0 to 60000000",
                         error);
  }
  return {};
}

}  // namespace

base::Status ValidateMarketDataScope(const MarketDataConfig& config,
                                     base::ConfigError& error) noexcept {
  error.Clear();
  if (!config.source_id.value) {
    return InvalidConfig("source_id", "must not be zero", error);
  }
  if (!config.instrument_count ||
      config.instrument_count > kMaxMarketDataInstruments) {
    return InvalidConfig("instruments", "must contain from 1 to 64 instruments",
                         error);
  }
  for (size_t i = 0; i < config.instrument_count; ++i) {
    const auto& definition = config.instruments[i].instrument;
    const auto status =
        ValidateInstrumentDefinition(definition, FieldPath(i, "").text, error);
    if (!status.ok()) {
      return status;
    }
    for (size_t j = 0; j < i; ++j) {
      if (config.instruments[j].instrument.id == definition.id) {
        return InvalidConfig(FieldPath(i, "symbol").text,
                             "duplicate venue and symbol", error);
      }
    }
  }
  return {};
}

base::Status ValidateMarketDataConfig(const MarketDataConfig& config,
                                      base::ConfigError& error) noexcept {
  if (config.adapter == MarketDataAdapter::kReplay &&
      config.instrument_count != 1) {
    return InvalidConfig("instruments",
                         "replay requires exactly one instrument", error);
  }
  auto status = ValidateMarketDataScope(config, error);
  if (!status.ok()) {
    return status;
  }
  if (config.adapter == MarketDataAdapter::kReplay) {
    if (config.runtime_directory[0] || config.stream[0] ||
        config.poll_interval_ns) {
      return InvalidConfig("adapter", "replay cannot use shm settings", error);
    }
    if (config.instrument_count != 1) {
      return InvalidConfig("instruments",
                           "replay requires exactly one instrument", error);
    }
    if (config.replay_path[0] != '/' ||
        !std::memchr(config.replay_path, '\0', sizeof(config.replay_path))) {
      return InvalidConfig("replay_path", "must be an absolute bounded path",
                           error);
    }
    if (config.replay_speed == 0 || config.replay_speed > 1000) {
      return InvalidConfig("replay_speed", "must be from 1 to 1000", error);
    }
    if (config.replay_tail_ns > 60000000000ULL) {
      return InvalidConfig("replay_tail_ms", "must be from 0 to 60000", error);
    }
    return {};
  }
  if (config.adapter != MarketDataAdapter::kShm) {
    return InvalidConfig("adapter", "unsupported receiver adapter", error);
  }
  if (config.replay_path[0] || config.replay_speed != 1 ||
      config.replay_tail_ns != 1000000000) {
    return InvalidConfig("adapter", "shm cannot use replay settings", error);
  }
  return ValidateShmEndpoint(config, error);
}

base::Status MarketDataConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  base::JsonConfigDocument document;
  auto result = document.Load(path);
  if (!result.ok()) {
    error_ = document.error();
    return result;
  }
  MarketDataConfig parsed;
  char catalog_reference[kMaxInstrumentCatalogPathLength + 1] = {};
  result = Parse(document, parsed, catalog_reference, error_);
  if (!result.ok()) {
    return result;
  }
  char catalog_path[kMaxInstrumentCatalogPathLength + 1] = {};
  result = base::ResolvePath(catalog_reference, catalog_path);
  if (!result.ok()) {
    error_.Set("instrument_catalog",
               result.code == base::ErrorCode::kIoError
                   ? "cannot resolve catalog path from working directory"
                   : "resolved catalog path exceeds output capacity");
    return result;
  }
  InstrumentCatalog catalog;
  result = catalog.Load(catalog_path);
  if (!result.ok()) {
    const auto& cause = catalog.error();
    char detail[sizeof(error_.message)];
    const int length =
        ::yquant::base::FormatTo(detail, sizeof(detail), "{}: {}: {}",
                                 catalog_path, cause.field_path, cause.message);
    error_.Set("instrument_catalog", detail);
    error_.truncated |= cause.truncated || length < 0 ||
                        static_cast<size_t>(length) >= sizeof(detail);
    return result;
  }
  for (size_t i = 0; i < parsed.instrument_count; ++i) {
    auto& definition = parsed.instruments[i].instrument;
    if (!catalog.Find(definition.id, definition)) {
      return InvalidConfig(FieldPath(i, "symbol").text,
                           "instrument is not defined in the catalog", error_);
    }
  }
  if (parsed.adapter == MarketDataAdapter::kReplay) {
    result = base::ResolvePath(parsed.replay_path, parsed.replay_path);
    if (!result.ok()) {
      error_.Set("replay_path", "cannot resolve path from working directory");
      return result;
    }
  }
  if (result.ok()) {
    result = ValidateMarketDataConfig(parsed, error_);
  }
  if (result.ok()) {
    config_ = parsed;
  }
  return result;
}

}  // namespace yquant::market
