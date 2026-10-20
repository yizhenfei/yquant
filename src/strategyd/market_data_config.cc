#include "strategyd/market_data_config.h"

#include <simdjson.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

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

// Loads one complete instrument into temporary storage. No parser views are
// retained in out; every field is required and unknown fields are rejected.
base::Status ParseInstrument(simdjson::ondemand::value value, std::size_t index,
                             MockInstrumentConfig& out,
                             strategy::ConfigError& error) noexcept {
  const FieldPath object_path(index, "");
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) return JsonError(json_error, object_path.text, error);
  MockInstrumentConfig parsed;
  std::string_view venue;
  std::string_view symbol;
  std::uint32_t seen = 0;
  constexpr std::string_view kFields[] = {
      "venue",   "symbol",    "price_decimals", "bid_price",
      "bid_qty", "ask_price", "ask_qty",        "price_step"};
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
      case 2: {
        std::uint64_t decimals;
        result = ReadUnsigned(field.value(), path.text, decimals, error);
        if (result.ok() && decimals > 6) {
          result = Invalid(path.text, "must be an integer from 0 to 6", error);
        }
        if (result.ok()) parsed.instrument.price_decimals = decimals;
        break;
      }
      case 3:
        result = ReadInteger(field.value(), path.text, parsed.bid_price, error);
        break;
      case 4:
        result = ReadInteger(field.value(), path.text, parsed.bid_qty, error);
        break;
      case 5:
        result = ReadInteger(field.value(), path.text, parsed.ask_price, error);
        break;
      case 6:
        result = ReadInteger(field.value(), path.text, parsed.ask_qty, error);
        break;
      case 7:
        result =
            ReadInteger(field.value(), path.text, parsed.price_step, error);
        break;
    }
    if (!result.ok()) return result;
  }
  for (std::size_t field = 0; field < std::size(kFields); ++field) {
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
  return {};
}

// Reads the bounded instrument array; the generic document already validated
// all nested values and decoded duplicate keys, including rejected entries.
base::Status ParseInstruments(simdjson::ondemand::value value,
                              MarketDataConfig& out,
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
    const auto status =
        ParseInstrument(item, out.instrument_count,
                        out.instruments[out.instrument_count], error);
    if (!status.ok()) return status;
    ++out.instrument_count;
  }
  return {};
}

// Applies the mock-source schema to fully validated UTF-8 JSON source.
base::Status Parse(const JsonConfigDocument& source, MarketDataConfig& out,
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
  constexpr std::string_view kFields[] = {"source_id", "adapter", "interval_ms",
                                          "instruments"};
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
    if (position == 0) {
      std::uint64_t source_id;
      result = ReadUnsigned(field.value(), key, source_id, error);
      if (result.ok() && (source_id == 0 || source_id > UINT32_MAX)) {
        result = Invalid(key, "must be an integer from 1 to UINT32_MAX", error);
      }
      if (result.ok()) out.source_id.value = source_id;
    } else if (position == 1) {
      std::string_view adapter;
      result = ReadText(field.value(), key, adapter, error);
      if (result.ok() && adapter != "mock")
        result = Invalid(key, "only mock is supported", error);
    } else if (position == 2) {
      std::uint64_t milliseconds;
      result = ReadUnsigned(field.value(), key, milliseconds, error);
      if (result.ok() && (milliseconds < 1 || milliseconds > 60000)) {
        result = Invalid(key, "must be an integer from 1 to 60000", error);
      }
      if (result.ok()) out.interval_ns = milliseconds * 1000000;
    } else {
      result = ParseInstruments(field.value(), out, error);
    }
    if (!result.ok()) return result;
  }
  for (std::size_t field = 0; field < std::size(kFields); ++field) {
    if ((seen & (1U << field)) == 0)
      return Invalid(kFields[field], "required field is missing", error);
  }
  return {};
}

}  // namespace

base::Status ValidateMarketDataConfig(const MarketDataConfig& config,
                                      strategy::ConfigError& error) noexcept {
  error.Clear();
  if (config.source_id.value == 0)
    return Invalid("source_id", "must not be zero", error);
  if (config.interval_ns < 1000000 || config.interval_ns > 60000000000ULL) {
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
    if (item.instrument.price_decimals > 6)
      return Invalid(FieldPath(i, "price_decimals").text, "must be from 0 to 6",
                     error);
    if (item.bid_price <= 0)
      return Invalid(FieldPath(i, "bid_price").text, "must be positive", error);
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
  result = Parse(document, parsed, error_);
  if (result.ok()) result = ValidateMarketDataConfig(parsed, error_);
  if (result.ok()) config_ = parsed;
  return result;
}

}  // namespace yquant::strategyd
