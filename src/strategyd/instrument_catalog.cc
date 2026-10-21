#include "strategyd/instrument_catalog.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "yquant/base/json_config_document.h"
#include "yquant/model/identifiers.h"

namespace yquant::strategyd {
namespace {

using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;

// Bounded diagnostic path for one catalog entry or one of its fields.
struct FieldPath {
  char text[sizeof(base::ConfigError::field_path)] = {};
  bool truncated = false;

  FieldPath(std::size_t index, std::string_view key = {}) noexcept {
    const int count =
        key.empty()
            ? std::snprintf(text, sizeof(text), "instruments[%zu]", index)
            : std::snprintf(text, sizeof(text), "instruments[%zu].%.*s", index,
                            static_cast<int>(key.size()), key.data());
    truncated = count < 0 || static_cast<std::size_t>(count) >= sizeof(text);
  }
};

// Parses one complete definition. Failure preserves out; decoded string views
// are borrowed only until the definition has been copied to out.
base::Status ParseInstrument(simdjson::ondemand::value value, std::size_t index,
                             model::InstrumentDefinition& out,
                             base::ConfigError& error) noexcept {
  const FieldPath object_path(index);
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) return JsonError(json_error, object_path.text, error);

  constexpr std::array<std::string_view, 3> kFields = {"venue", "symbol",
                                                       "price_decimals"};
  std::array<bool, kFields.size()> seen = {};
  std::string_view venue;
  std::string_view symbol;
  std::uint64_t decimals = 0;
  for (auto field_result : object) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, object_path.text, error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, object_path.text, error);
    const FieldPath path(index, key);
    std::size_t position = 0;
    while (position < kFields.size() && kFields[position] != key) ++position;
    if (position == kFields.size()) {
      const auto result =
          InvalidConfig(path.text, "unknown instrument field", error);
      error.truncated |= path.truncated;
      return result;
    }
    seen[position] = true;
    if (key == "venue")
      json_error = field.value().get_string().get(venue);
    else if (key == "symbol")
      json_error = field.value().get_string().get(symbol);
    else
      json_error = field.value().get_uint64().get(decimals);
    if (json_error) return JsonError(json_error, path.text, error);
  }
  for (std::size_t field = 0; field < kFields.size(); ++field) {
    if (!seen[field])
      return InvalidConfig(FieldPath(index, kFields[field]).text,
                           "required field is missing", error);
  }
  if (decimals > model::kMaxPriceDecimals)
    return InvalidConfig(FieldPath(index, "price_decimals").text,
                         "must be an integer from 0 to 6", error);

  model::InstrumentDefinition parsed;
  if (model::MakeInstrumentId(venue, symbol, parsed.id) !=
      base::ErrorCode::kOk) {
    if (venue != "SSE" && venue != "SZSE" && venue != "BSE")
      return InvalidConfig(FieldPath(index, "venue").text, "unsupported venue",
                           error);
    return InvalidConfig(FieldPath(index, "symbol").text,
                         "must contain exactly six ASCII digits", error);
  }
  parsed.venue = venue == "SSE"    ? model::Venue::kSse
                 : venue == "SZSE" ? model::Venue::kSzse
                                   : model::Venue::kBse;
  std::memcpy(parsed.symbol, symbol.data(), model::kSymbolLength);
  parsed.price_decimals = static_cast<std::uint8_t>(decimals);
  out = parsed;
  return {};
}

// Fills only caller-owned storage. Failure may leave a parsed prefix and count;
// Load publishes them only on success. JsonConfigDocument already checked all
// decoded keys for duplicates.
base::Status ParseCatalog(const base::JsonConfigDocument& source,
                          std::span<model::InstrumentDefinition> definitions,
                          std::size_t& count,
                          base::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const auto parsed = ParseConfigObject(source, parser, document, root, error);
  if (!parsed.ok()) return parsed;
  simdjson::error_code json_error;
  bool has_instruments = false;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, "$", error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, "$", error);
    if (key != "instruments")
      return InvalidConfig(key, "unknown root field", error);
    has_instruments = true;
    simdjson::ondemand::array instruments;
    json_error = field.value().get_array().get(instruments);
    if (json_error) return JsonError(json_error, key, error);
    for (auto item_result : instruments) {
      if (count == definitions.size())
        return InvalidConfig("instruments",
                             "at most 16384 instruments are allowed", error);
      simdjson::ondemand::value item;
      json_error = std::move(item_result).get(item);
      if (json_error) return JsonError(json_error, "instruments", error);
      const auto result =
          ParseInstrument(item, count, definitions[count], error);
      if (!result.ok()) return result;
      ++count;
    }
  }
  if (!has_instruments)
    return InvalidConfig("instruments", "required field is missing", error);
  if (count == 0)
    return InvalidConfig("instruments", "must contain at least one instrument",
                         error);
  return {};
}

}  // namespace

base::Status InstrumentCatalog::Load(const char* path) noexcept {
  size_ = 0;
  error_.Clear();
  base::JsonConfigDocument document;
  auto result = document.Load(path);
  if (!result.ok()) {
    error_ = document.error();
    return result;
  }
  if (definitions_ == nullptr) {
    definitions_.reset(
        new (std::nothrow)
            model::InstrumentDefinition[kMaxInstrumentCatalogEntries]);
    if (definitions_ == nullptr) {
      error_.Set("instruments", "cannot allocate instrument catalog storage");
      return {base::ErrorCode::kResourceExhausted, {}};
    }
  }
  std::size_t count = 0;
  result =
      ParseCatalog(document, {definitions_.get(), kMaxInstrumentCatalogEntries},
                   count, error_);
  if (!result.ok()) return result;
  // Keep one contiguous allocation and logarithmic, allocation-free lookups.
  std::sort(definitions_.get(), definitions_.get() + count,
            [](const auto& left, const auto& right) {
              return left.id.value < right.id.value;
            });
  for (std::size_t index = 1; index < count; ++index) {
    if (definitions_[index - 1].id == definitions_[index].id)
      return InvalidConfig("instruments", "duplicate venue and symbol", error_);
  }
  size_ = count;
  return {};
}

bool InstrumentCatalog::Find(model::InstrumentId id,
                             model::InstrumentDefinition& out) const noexcept {
  if (size_ == 0) return false;
  const auto end = definitions_.get() + size_;
  const auto found = std::lower_bound(
      definitions_.get(), end, id, [](const auto& definition, auto wanted) {
        return definition.id.value < wanted.value;
      });
  if (found == end || found->id != id) return false;
  out = *found;
  return true;
}

}  // namespace yquant::strategyd
