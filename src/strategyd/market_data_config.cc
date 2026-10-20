#include "strategyd/market_data_config.h"

#include <simdjson.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "strategyd/instrument_catalog.h"
#include "strategyd/json_config_document.h"

namespace yquant::strategyd {
namespace {

// Bounded diagnostic path for a field in the instrument array.
struct FieldPath {
  char text[256] = {};
  bool truncated = false;

  FieldPath(std::size_t index, std::string_view key) noexcept {
    const int count =
        std::snprintf(text, sizeof(text), "instruments[%zu].%.*s", index,
                      static_cast<int>(key.size()), key.data());
    truncated = count < 0 || static_cast<std::size_t>(count) >= sizeof(text);
  }
};

// Replaces the supplied error with an owned schema diagnostic.
base::Status Invalid(std::string_view path, std::string_view reason,
                     strategy::ConfigError& error) noexcept {
  error.Set(path, reason);
  return {base::ErrorCode::kInvalidArgument, {}};
}

// Preserves the field path and distinguishes allocation from JSON/type errors.
base::Status JsonError(simdjson::error_code code, std::string_view path,
                       strategy::ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Reads an exact signed integer; string, fractional and overflowing values
// fail.
base::Status ReadInteger(simdjson::ondemand::value value, std::string_view path,
                         std::int64_t& out,
                         strategy::ConfigError& error) noexcept {
  const auto code = value.get_int64().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

// Reads unsigned integers without any intermediate floating-point conversion.
base::Status ReadUnsigned(simdjson::ondemand::value value,
                          std::string_view path, std::uint64_t& out,
                          strategy::ConfigError& error) noexcept {
  const auto code = value.get_uint64().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

// Borrows the decoded string only while the schema parser is alive.
base::Status ReadText(simdjson::ondemand::value value, std::string_view path,
                      std::string_view& out,
                      strategy::ConfigError& error) noexcept {
  const auto code = value.get_string().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

// Maps only supported venue codes; empty denotes invalid typed definitions.
std::string_view VenueName(model::Venue venue) noexcept {
  switch (venue) {
    case model::Venue::kSse:
      return "SSE";
    case model::Venue::kSzse:
      return "SZSE";
    case model::Venue::kBse:
      return "BSE";
    case model::Venue::kUnknown:
      return {};
  }
  return {};
}

// Loads an instrument reference and optional mock seed into temporary storage.
// Metadata is resolved from the catalog after root parsing. No parser views
// are retained, and field order does not affect the selected adapter schema.
base::Status ParseInstrument(simdjson::ondemand::value value, std::size_t index,
                             MockInstrumentConfig& out,
                             std::uint32_t& field_mask,
                             strategy::ConfigError& error) noexcept {
  const FieldPath object_path(index, "");
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) return JsonError(json_error, object_path.text, error);
  MockInstrumentConfig parsed;
  std::string_view venue;
  std::string_view symbol;
  std::uint32_t seen = 0;
  constexpr std::string_view kFields[] = {"venue",     "symbol",    "bid_price",
                                          "bid_qty",   "ask_price", "ask_qty",
                                          "price_step"};
  for (auto field_result : object) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, object_path.text, error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, object_path.text, error);
    const FieldPath path(index, key);
    std::size_t position = 0;
    while (position < std::size(kFields) && kFields[position] != key)
      ++position;
    if (position == std::size(kFields)) {
      const auto result = Invalid(path.text, "unknown instrument field", error);
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
      case 2:
        result = ReadInteger(field.value(), path.text, parsed.bid_price, error);
        break;
      case 3:
        result = ReadInteger(field.value(), path.text, parsed.bid_qty, error);
        break;
      case 4:
        result = ReadInteger(field.value(), path.text, parsed.ask_price, error);
        break;
      case 5:
        result = ReadInteger(field.value(), path.text, parsed.ask_qty, error);
        break;
      case 6:
        result =
            ReadInteger(field.value(), path.text, parsed.price_step, error);
        break;
    }
    if (!result.ok()) return result;
  }
  for (std::size_t field = 0; field < 2; ++field) {
    if ((seen & (1U << field)) == 0) {
      return Invalid(FieldPath(index, kFields[field]).text,
                     "required field is missing", error);
    }
  }
  if (venue == "SSE")
    parsed.instrument.venue = model::Venue::kSse;
  else if (venue == "SZSE")
    parsed.instrument.venue = model::Venue::kSzse;
  else if (venue == "BSE")
    parsed.instrument.venue = model::Venue::kBse;
  else
    return Invalid(FieldPath(index, "venue").text, "unsupported venue", error);
  const auto id_result =
      model::MakeInstrumentId(venue, symbol, parsed.instrument.id);
  if (id_result != base::ErrorCode::kOk) {
    return Invalid(FieldPath(index, "symbol").text,
                   "must contain exactly six ASCII digits", error);
  }
  std::memcpy(parsed.instrument.symbol, symbol.data(),
              sizeof(parsed.instrument.symbol));
  out = parsed;
  field_mask = seen;
  return {};
}

// Reads the bounded instrument array; the generic document already validated
// all nested values and decoded duplicate keys, including rejected entries.
base::Status ParseInstruments(
    simdjson::ondemand::value value, MarketDataConfig& out,
    std::array<std::uint32_t, kMaxMarketDataInstruments>& masks,
    strategy::ConfigError& error) noexcept {
  simdjson::ondemand::array array;
  auto json_error = value.get_array().get(array);
  if (json_error) return JsonError(json_error, "instruments", error);
  for (auto result : array) {
    if (out.instrument_count == kMaxMarketDataInstruments) {
      return Invalid("instruments", "at most 64 instruments are allowed",
                     error);
    }
    simdjson::ondemand::value item;
    json_error = std::move(result).get(item);
    if (json_error) return JsonError(json_error, "instruments", error);
    const auto status = ParseInstrument(item, out.instrument_count,
                                        out.instruments[out.instrument_count],
                                        masks[out.instrument_count], error);
    if (!status.ok()) return status;
    ++out.instrument_count;
  }
  return {};
}

// Copies a bounded endpoint value while rejecting embedded NUL bytes.
template <std::size_t kSize>
base::Status ReadOwnedText(simdjson::ondemand::value value,
                           std::string_view path, char (&out)[kSize],
                           strategy::ConfigError& error) noexcept {
  std::string_view text;
  auto status = ReadText(value, path, text, error);
  if (!status.ok()) return status;
  if (text.empty() || text.size() >= kSize ||
      text.find('\0') != std::string_view::npos) {
    return Invalid(path, "must be nonempty, bounded text without NUL", error);
  }
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return {};
}

// Applies one adapter's schema after parsing all fields, allowing arbitrary
// JSON field order while rejecting fields belonging to the other adapter.
base::Status Parse(
    const JsonConfigDocument& source, MarketDataConfig& out,
    char (&catalog_reference)[kMaxInstrumentCatalogPathLength + 1],
    strategy::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  auto json_error =
      parser.allocate(source.source().size(), kMaxConfigDepth + 4);
  if (json_error) return JsonError(json_error, "$", error);
  simdjson::ondemand::document document;
  json_error = parser.iterate(source.source(), source.capacity()).get(document);
  if (json_error) return JsonError(json_error, "$", error);
  simdjson::ondemand::object root;
  json_error = document.get_object().get(root);
  if (json_error) return JsonError(json_error, "$", error);
  constexpr std::string_view kFields[] = {
      "source_id",         "adapter", "interval_ms",      "instruments",
      "runtime_directory", "stream",  "poll_interval_us", "instrument_catalog"};
  constexpr std::string_view kInstrumentFields[] = {
      "venue",     "symbol",  "bid_price", "bid_qty",
      "ask_price", "ask_qty", "price_step"};
  std::array<std::uint32_t, kMaxMarketDataInstruments> masks = {};
  std::uint32_t seen = 0;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, "$", error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, "$", error);
    std::size_t position = 0;
    while (position < std::size(kFields) && kFields[position] != key)
      ++position;
    if (position == std::size(kFields))
      return Invalid(key, "unknown root field", error);
    seen |= 1U << position;
    base::Status result;
    switch (position) {
      case 0: {
        std::uint64_t source_id;
        result = ReadUnsigned(field.value(), key, source_id, error);
        if (result.ok() && (source_id == 0 || source_id > UINT32_MAX)) {
          result =
              Invalid(key, "must be an integer from 1 to UINT32_MAX", error);
        }
        if (result.ok()) out.source_id.value = source_id;
        break;
      }
      case 1: {
        std::string_view adapter;
        result = ReadText(field.value(), key, adapter, error);
        if (result.ok()) {
          if (adapter == "mock")
            out.adapter = MarketDataAdapter::kMock;
          else if (adapter == "shm")
            out.adapter = MarketDataAdapter::kShm;
          else
            result = Invalid(key, "must be mock or shm", error);
        }
        break;
      }
      case 2: {
        std::uint64_t milliseconds;
        result = ReadUnsigned(field.value(), key, milliseconds, error);
        if (result.ok() && (milliseconds < 1 || milliseconds > 60000)) {
          result = Invalid(key, "must be an integer from 1 to 60000", error);
        }
        if (result.ok()) out.interval_ns = milliseconds * 1000000;
        break;
      }
      case 3:
        result = ParseInstruments(field.value(), out, masks, error);
        break;
      case 4:
        result =
            ReadOwnedText(field.value(), key, out.runtime_directory, error);
        break;
      case 5:
        result = ReadOwnedText(field.value(), key, out.stream, error);
        break;
      case 6: {
        std::uint64_t microseconds;
        result = ReadUnsigned(field.value(), key, microseconds, error);
        if (result.ok() && microseconds > 60000000) {
          result = Invalid(key, "must be from 0 to 60000000", error);
        }
        if (result.ok()) out.poll_interval_ns = microseconds * 1000;
        break;
      }
      case 7:
        result = ReadOwnedText(field.value(), key, catalog_reference, error);
        break;
    }
    if (!result.ok()) return result;
  }
  const bool mock = out.adapter == MarketDataAdapter::kMock;
  // Source identity, adapter, catalog and selected instruments are common.
  // Transport settings belong only to their selected adapter.
  constexpr std::uint32_t kCommonFields =
      (1U << 0) | (1U << 1) | (1U << 3) | (1U << 7);
  constexpr std::uint32_t kMockFields = kCommonFields | (1U << 2);
  constexpr std::uint32_t kShmFields = kCommonFields | (1U << 4) | (1U << 5);
  const std::uint32_t required = mock ? kMockFields : kShmFields;
  const std::uint32_t allowed = mock ? kMockFields : kShmFields | (1U << 6);
  for (std::size_t field = 0; field < std::size(kFields); ++field) {
    if ((required & (1U << field)) != 0 && (seen & (1U << field)) == 0)
      return Invalid(kFields[field], "required field is missing", error);
    if ((allowed & (1U << field)) == 0 && (seen & (1U << field)) != 0)
      return Invalid(kFields[field], "field is not valid for this adapter",
                     error);
  }
  for (std::size_t i = 0; i < out.instrument_count; ++i) {
    for (std::size_t field = 2; field < std::size(kInstrumentFields); ++field) {
      const bool present = (masks[i] & (1U << field)) != 0;
      if (mock && !present) {
        return Invalid(FieldPath(i, kInstrumentFields[field]).text,
                       "required field is missing", error);
      }
      if (!mock && present) {
        return Invalid(FieldPath(i, kInstrumentFields[field]).text,
                       "field is not valid for shm adapter", error);
      }
    }
  }
  return {};
}

// Verifies endpoint text without touching the filesystem or allocating.
base::Status ValidateShmEndpoint(const MarketDataConfig& config,
                                 strategy::ConfigError& error) noexcept {
  const auto* path_end = static_cast<const char*>(std::memchr(
      config.runtime_directory, '\0', sizeof(config.runtime_directory)));
  if (path_end == nullptr || config.runtime_directory[0] != '/') {
    return Invalid("runtime_directory", "must be an absolute bounded path",
                   error);
  }
  const std::string_view path(config.runtime_directory,
                              path_end - config.runtime_directory);
  std::size_t offset = 0;
  while (offset <= path.size()) {
    const auto end = path.find('/', offset);
    const auto part = path.substr(
        offset, end == path.npos ? path.size() - offset : end - offset);
    if (part == "..")
      return Invalid("runtime_directory", "parent traversal is not allowed",
                     error);
    if (end == path.npos) break;
    offset = end + 1;
  }
  const auto* stream_end = static_cast<const char*>(
      std::memchr(config.stream, '\0', sizeof(config.stream)));
  if (stream_end == nullptr || stream_end == config.stream)
    return Invalid("stream", "must contain from 1 to 64 ASCII characters",
                   error);
  const auto alnum = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(config.stream[0]))
    return Invalid("stream", "must start with an ASCII letter or digit", error);
  for (const char* p = config.stream; p != stream_end; ++p) {
    if (!alnum(*p) && *p != '_' && *p != '-' && *p != '.')
      return Invalid("stream", "contains an invalid character", error);
  }
  if (config.poll_interval_ns > 60000000000ULL)
    return Invalid("poll_interval_us", "must be from 0 to 60000000", error);
  return {};
}

}  // namespace

base::Status ValidateMarketDataConfig(const MarketDataConfig& config,
                                      strategy::ConfigError& error) noexcept {
  error.Clear();
  if (config.source_id.value == 0)
    return Invalid("source_id", "must not be zero", error);
  if (config.adapter != MarketDataAdapter::kMock &&
      config.adapter != MarketDataAdapter::kShm)
    return Invalid("adapter", "unsupported adapter", error);
  const bool mock = config.adapter == MarketDataAdapter::kMock;
  if (!mock) {
    const auto endpoint = ValidateShmEndpoint(config, error);
    if (!endpoint.ok()) return endpoint;
    if (config.interval_ns != 0)
      return Invalid("interval_ms", "field is not valid for shm adapter",
                     error);
  } else if (config.runtime_directory[0] != '\0' || config.stream[0] != '\0' ||
             config.poll_interval_ns != 0) {
    return Invalid("adapter", "mock adapter cannot use shm configuration",
                   error);
  }
  if (mock &&
      (config.interval_ns < 1000000 || config.interval_ns > 60000000000ULL)) {
    return Invalid("interval_ms",
                   "interval must be from 1 to 60000 milliseconds", error);
  }
  if (config.instrument_count == 0 ||
      config.instrument_count > kMaxMarketDataInstruments) {
    return Invalid("instruments", "must contain from 1 to 64 instruments",
                   error);
  }
  for (std::size_t i = 0; i < config.instrument_count; ++i) {
    const auto& item = config.instruments[i];
    model::InstrumentId expected;
    if (model::MakeInstrumentId(VenueName(item.instrument.venue),
                                item.instrument.symbol_view(),
                                expected) != base::ErrorCode::kOk ||
        expected != item.instrument.id) {
      return Invalid(FieldPath(i, "symbol").text, "invalid instrument identity",
                     error);
    }
    if (item.instrument.price_decimals > model::kMaxPriceDecimals)
      return Invalid(FieldPath(i, "price_decimals").text, "must be from 0 to 6",
                     error);
    if (!mock &&
        (item.bid_price != 0 || item.bid_qty != 0 || item.ask_price != 0 ||
         item.ask_qty != 0 || item.price_step != 0)) {
      return Invalid(FieldPath(i, "bid_price").text,
                     "mock prices are not valid for shm adapter", error);
    }
    if (mock) {
      if (item.bid_price <= 0)
        return Invalid(FieldPath(i, "bid_price").text, "must be positive",
                       error);
      if (item.ask_price <= item.bid_price)
        return Invalid(FieldPath(i, "ask_price").text,
                       "must be greater than bid_price", error);
      if (item.bid_qty <= 0)
        return Invalid(FieldPath(i, "bid_qty").text, "must be positive", error);
      if (item.ask_qty <= 0)
        return Invalid(FieldPath(i, "ask_qty").text, "must be positive", error);
      if (item.price_step < 0)
        return Invalid(FieldPath(i, "price_step").text, "must be nonnegative",
                       error);
      if (item.price_step >
              std::numeric_limits<std::int64_t>::max() - item.bid_price ||
          item.price_step >
              std::numeric_limits<std::int64_t>::max() - item.ask_price) {
        return Invalid(FieldPath(i, "price_step").text,
                       "price plus step overflows int64", error);
      }
    }
    for (std::size_t previous = 0; previous < i; ++previous) {
      if (config.instruments[previous].instrument.id == item.instrument.id) {
        return Invalid(FieldPath(i, "symbol").text,
                       "duplicate venue and symbol", error);
      }
    }
  }
  return {};
}

base::Status MarketDataConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  JsonConfigDocument document;
  auto result = document.Load(path);
  if (!result.ok()) {
    error_ = document.error();
    return result;
  }
  MarketDataConfig parsed;
  char catalog_reference[kMaxInstrumentCatalogPathLength + 1] = {};
  result = Parse(document, parsed, catalog_reference, error_);
  if (!result.ok()) return result;
  char catalog_path[kMaxInstrumentCatalogPathLength + 1] = {};
  result = ResolveInstrumentCatalogPath(path, catalog_reference, catalog_path,
                                        error_);
  if (!result.ok()) return result;
  InstrumentCatalogLoader catalog;
  result = catalog.Load(catalog_path);
  if (!result.ok()) {
    const auto& cause = catalog.error();
    char detail[sizeof(error_.message)];
    const int length =
        std::snprintf(detail, sizeof(detail), "%s: %s: %s", catalog_path,
                      cause.field_path, cause.message);
    error_.Set("instrument_catalog", detail);
    error_.truncated |= cause.truncated || length < 0 ||
                        static_cast<std::size_t>(length) >= sizeof(detail);
    return result;
  }
  for (std::size_t i = 0; i < parsed.instrument_count; ++i) {
    auto& definition = parsed.instruments[i].instrument;
    if (!catalog.Find(definition.id, definition)) {
      return Invalid(FieldPath(i, "symbol").text,
                     "instrument is not defined in the catalog", error_);
    }
  }
  if (result.ok()) result = ValidateMarketDataConfig(parsed, error_);
  if (result.ok()) config_ = parsed;
  return result;
}

}  // namespace yquant::strategyd
