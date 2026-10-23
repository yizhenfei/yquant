#include "yquant/md/mock_quote_config.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
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
#include "yquant/md/instrument_catalog.h"

namespace yquant::md {
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

// Reads an exact signed integer; string, fractional and overflowing values
// fail.
base::Status ReadInteger(simdjson::ondemand::value value, std::string_view path,
                         int64_t& out, base::ConfigError& error) noexcept {
  const auto code = value.get_int64().get(out);
  return code ? JsonError(code, path, error) : base::Status{};
}

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

// Loads one instrument reference and quote seed into temporary storage.
// Metadata is resolved from the catalog; no JSON views survive parsing.
base::Status ParseInstrument(simdjson::ondemand::value value, size_t index,
                             MockInstrumentConfig& out, uint32_t& field_mask,
                             base::ConfigError& error) noexcept {
  const FieldPath object_path(index, "");
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) {
    return JsonError(json_error, object_path.text, error);
  }
  MockInstrumentConfig parsed;
  std::string_view venue;
  std::string_view symbol;
  uint32_t seen = 0;
  constexpr std::string_view kFields[] = {"venue",     "symbol",    "bid_price",
                                          "bid_qty",   "ask_price", "ask_qty",
                                          "price_step"};
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
  field_mask = seen;
  return {};
}

// Reads the bounded instrument array; the generic document already validated
// all nested values and decoded duplicate keys, including rejected entries.
base::Status ParseInstruments(
    simdjson::ondemand::value value, MockQuoteConfig& out,
    std::array<uint32_t, kMaxMockQuoteInstruments>& masks,
    base::ConfigError& error) noexcept {
  simdjson::ondemand::array array;
  auto json_error = value.get_array().get(array);
  if (json_error) {
    return JsonError(json_error, "instruments", error);
  }
  for (auto result : array) {
    if (out.instrument_count == kMaxMockQuoteInstruments) {
      return InvalidConfig("instruments", "at most 64 instruments are allowed",
                           error);
    }
    simdjson::ondemand::value item;
    json_error = std::move(result).get(item);
    if (json_error) {
      return JsonError(json_error, "instruments", error);
    }
    const auto status = ParseInstrument(item, out.instrument_count,
                                        out.instruments[out.instrument_count],
                                        masks[out.instrument_count], error);
    if (!status.ok()) {
      return status;
    }
    ++out.instrument_count;
  }
  return {};
}

// Parses only producer settings. All fields are required and may appear in
// any order; generic JSON validation already rejected decoded duplicate keys.
base::Status Parse(
    const base::JsonConfigDocument& source, MockQuoteConfig& out,
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
  constexpr std::string_view kFields[] = {"source_id", "adapter", "interval_ms",
                                          "instruments", "instrument_catalog"};
  constexpr std::string_view kInstrumentFields[] = {
      "venue",     "symbol",  "bid_price", "bid_qty",
      "ask_price", "ask_qty", "price_step"};
  std::array<uint32_t, kMaxMockQuoteInstruments> masks = {};
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
          if (adapter != "mock") {
            result = InvalidConfig(key, "must be mock", error);
          }
        }
        break;
      }
      case 2: {
        uint64_t milliseconds;
        result = ReadUnsigned(field.value(), key, milliseconds, error);
        if (result.ok() && (milliseconds < 1 || milliseconds > 60000)) {
          result =
              InvalidConfig(key, "must be an integer from 1 to 60000", error);
        }
        if (result.ok()) {
          out.interval_ns = milliseconds * 1000000;
        }
        break;
      }
      case 3:
        result = ParseInstruments(field.value(), out, masks, error);
        break;
      case 4:
        result = ReadConfigText(field.value(), catalog_reference, key, error,
                                "must be nonempty, bounded text without NUL");
        break;
    }
    if (!result.ok()) {
      return result;
    }
  }
  constexpr uint32_t kRequiredFields = (1U << std::size(kFields)) - 1;
  for (size_t field = 0; field < std::size(kFields); ++field) {
    if ((kRequiredFields & (1U << field)) && !(seen & (1U << field))) {
      return InvalidConfig(kFields[field], "required field is missing", error);
    }
  }
  for (size_t i = 0; i < out.instrument_count; ++i) {
    for (size_t field = 2; field < std::size(kInstrumentFields); ++field) {
      if (!(masks[i] & (1U << field))) {
        return InvalidConfig(FieldPath(i, kInstrumentFields[field]).text,
                             "required field is missing", error);
      }
    }
  }
  return {};
}

}  // namespace

base::Status ValidateMockQuoteConfig(const MockQuoteConfig& config,
                                     base::ConfigError& error) noexcept {
  error.Clear();
  if (config.source_id.value == 0) {
    return InvalidConfig("source_id", "must not be zero", error);
  }
  if (config.interval_ns < 1000000 || config.interval_ns > 60000000000ULL) {
    return InvalidConfig(
        "interval_ms", "interval must be from 1 to 60000 milliseconds", error);
  }
  if (config.instrument_count == 0 ||
      config.instrument_count > kMaxMockQuoteInstruments) {
    return InvalidConfig("instruments", "must contain from 1 to 64 instruments",
                         error);
  }
  for (size_t i = 0; i < config.instrument_count; ++i) {
    const auto& item = config.instruments[i];
    const auto definition_status = ValidateInstrumentDefinition(
        item.instrument, FieldPath(i, "").text, error);
    if (!definition_status.ok()) {
      return definition_status;
    }
    if (item.bid_price <= 0) {
      return InvalidConfig(FieldPath(i, "bid_price").text, "must be positive",
                           error);
    }
    if (item.ask_price <= item.bid_price) {
      return InvalidConfig(FieldPath(i, "ask_price").text,
                           "must be greater than bid_price", error);
    }
    if (item.bid_qty <= 0) {
      return InvalidConfig(FieldPath(i, "bid_qty").text, "must be positive",
                           error);
    }
    if (item.ask_qty <= 0) {
      return InvalidConfig(FieldPath(i, "ask_qty").text, "must be positive",
                           error);
    }
    if (item.price_step < 0) {
      return InvalidConfig(FieldPath(i, "price_step").text,
                           "must be nonnegative", error);
    }
    if (item.bid_price % item.instrument.price_tick ||
        item.ask_price % item.instrument.price_tick ||
        item.price_step % item.instrument.price_tick ||
        item.bid_qty % item.instrument.quantity_step ||
        item.ask_qty % item.instrument.quantity_step) {
      return InvalidConfig(FieldPath(i, "bid_price").text,
                           "quote is not aligned to tick/quantity units",
                           error);
    }
    if (item.price_step >
            std::numeric_limits<int64_t>::max() - item.bid_price ||
        item.price_step >
            std::numeric_limits<int64_t>::max() - item.ask_price) {
      return InvalidConfig(FieldPath(i, "price_step").text,
                           "price plus step overflows int64", error);
    }
    for (size_t previous = 0; previous < i; ++previous) {
      if (config.instruments[previous].instrument.id == item.instrument.id) {
        return InvalidConfig(FieldPath(i, "symbol").text,
                             "duplicate venue and symbol", error);
      }
    }
  }
  return {};
}

base::Status MockQuoteConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  base::JsonConfigDocument document;
  auto result = document.Load(path);
  if (!result.ok()) {
    error_ = document.error();
    return result;
  }
  MockQuoteConfig parsed;
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
  if (result.ok()) {
    result = ValidateMockQuoteConfig(parsed, error_);
  }
  if (result.ok()) {
    config_ = parsed;
  }
  return result;
}

}  // namespace yquant::md
