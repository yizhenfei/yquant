#include "strategyd/instrument_catalog.h"

#include <simdjson.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <system_error>
#include <utility>

#include "strategyd/json_config_document.h"
#include "yquant/model/identifiers.h"

namespace yquant::strategyd {
namespace {

// Copies a schema diagnostic without retaining the supplied views.
base::Status Invalid(std::string_view path, std::string_view reason,
                     strategy::ConfigError& error) noexcept {
  error.Set(path, reason);
  return {base::ErrorCode::kInvalidArgument, {}};
}

// Distinguishes parser allocation failures from malformed or mistyped JSON.
base::Status JsonError(simdjson::error_code code, std::string_view path,
                       strategy::ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Bounded diagnostic path for one catalog entry or one of its fields.
struct FieldPath {
  char text[sizeof(strategy::ConfigError::field_path)] = {};
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

constexpr std::string_view kInstrumentFields[] = {
    "venue",
    "symbol",
    "price_decimals",
    "instrument_type",
    "price_tick",
    "quantity_step",
    "contract_multiplier",
    "margin_rate_ppm",
    "open_fee_per_lot",
    "close_today_fee_per_lot",
    "close_yesterday_fee_per_lot"};
constexpr std::size_t kFirstContractField = 6;

// Parses complete shared metadata, retaining no parser views. Futures must
// explicitly provide every contract specification; equity keeps unit defaults.
base::Status ParseInstrument(simdjson::ondemand::value value, std::size_t index,
                             model::InstrumentDefinition& out,
                             strategy::ConfigError& error) noexcept {
  const FieldPath object_path(index, "");
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) return JsonError(json_error, object_path.text, error);
  model::InstrumentDefinition parsed;
  std::string_view venue;
  std::string_view symbol;
  std::uint32_t seen = 0;
  for (auto field_result : object) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, object_path.text, error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, object_path.text, error);
    const FieldPath path(index, key);
    std::size_t position = 0;
    while (position < std::size(kInstrumentFields) &&
           kInstrumentFields[position] != key)
      ++position;
    if (position == std::size(kInstrumentFields)) {
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
        if (result.ok() && decimals > model::kMaxPriceDecimals) {
          result = Invalid(path.text, "must be an integer from 0 to 6", error);
        }
        if (result.ok()) parsed.price_decimals = decimals;
        break;
      }
      case 3: {
        std::string_view type;
        result = ReadText(field.value(), path.text, type, error);
        if (result.ok()) {
          if (type == "equity")
            parsed.type = model::InstrumentType::kEquity;
          else if (type == "futures")
            parsed.type = model::InstrumentType::kFutures;
          else
            result = Invalid(path.text, "must be equity or futures", error);
        }
        break;
      }
      case 4:
        result =
            ReadInteger(field.value(), path.text, parsed.price_tick, error);
        break;
      case 5:
        result =
            ReadInteger(field.value(), path.text, parsed.quantity_step, error);
        break;
      case 6:
        result = ReadInteger(field.value(), path.text,
                             parsed.contract_multiplier, error);
        break;
      case 7:
        result = ReadInteger(field.value(), path.text, parsed.margin_rate_ppm,
                             error);
        break;
      case 8:
        result = ReadInteger(field.value(), path.text, parsed.open_fee_per_lot,
                             error);
        break;
      case 9:
        result = ReadInteger(field.value(), path.text,
                             parsed.close_today_fee_per_lot, error);
        break;
      case 10:
        result = ReadInteger(field.value(), path.text,
                             parsed.close_yesterday_fee_per_lot, error);
        break;
    }
    if (!result.ok()) return result;
  }
  for (std::size_t field = 0; field < 3; ++field) {
    if ((seen & (1U << field)) == 0) {
      return Invalid(FieldPath(index, kInstrumentFields[field]).text,
                     "required field is missing", error);
    }
  }
  if (model::ParseVenue(venue) == model::Venue::kUnknown)
    return Invalid(FieldPath(index, "venue").text, "unsupported venue", error);
  const auto id_result = model::MakeInstrumentId(venue, symbol, parsed.id);
  if (id_result != base::ErrorCode::kOk) {
    return Invalid(FieldPath(index, "symbol").text,
                   "invalid canonical symbol for venue", error);
  }
  parsed.venue = static_cast<model::Venue>(parsed.id.value >> 32);
  std::memcpy(parsed.symbol, symbol.data(), symbol.size());
  if (parsed.type == model::InstrumentType::kFutures) {
    for (std::size_t field = 4; field < std::size(kInstrumentFields); ++field) {
      if ((seen & (1U << field)) == 0) {
        return Invalid(FieldPath(index, kInstrumentFields[field]).text,
                       "required futures specification is missing", error);
      }
    }
  } else {
    for (std::size_t field = kFirstContractField;
         field < std::size(kInstrumentFields); ++field) {
      if ((seen & (1U << field)) != 0) {
        return Invalid(FieldPath(index, kInstrumentFields[field]).text,
                       "field is only valid for futures", error);
      }
    }
  }
  const auto result =
      ValidateInstrumentDefinition(parsed, object_path.text, error);
  if (!result.ok()) return result;
  out = parsed;
  return {};
}

// Fills only caller-owned storage. Failure may leave a parsed prefix and count;
// Load publishes them only on success. JsonConfigDocument already checked all
// decoded keys for duplicates.
base::Status ParseCatalog(const JsonConfigDocument& source,
                          std::span<model::InstrumentDefinition> definitions,
                          std::size_t& count,
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
  bool has_instruments = false;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, "$", error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, "$", error);
    if (key != "instruments") return Invalid(key, "unknown root field", error);
    has_instruments = true;
    simdjson::ondemand::array instruments;
    json_error = field.value().get_array().get(instruments);
    if (json_error) return JsonError(json_error, key, error);
    for (auto item_result : instruments) {
      if (count == definitions.size())
        return Invalid("instruments", "at most 16384 instruments are allowed",
                       error);
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
    return Invalid("instruments", "required field is missing", error);
  if (count == 0)
    return Invalid("instruments", "must contain at least one instrument",
                   error);
  return {};
}

}  // namespace

base::Status ValidateInstrumentDefinition(
    const model::InstrumentDefinition& definition, std::string_view field_path,
    strategy::ConfigError& error) noexcept {
  const auto invalid = [&](std::string_view field, std::string_view reason) {
    char path[sizeof(strategy::ConfigError::field_path)] = {};
    std::snprintf(path, sizeof(path), "%.*s%s%.*s",
                  static_cast<int>(field_path.size()), field_path.data(),
                  field_path.empty() || field_path.back() == '.' ? "" : ".",
                  static_cast<int>(field.size()), field.data());
    return Invalid(path, reason, error);
  };
  model::InstrumentId expected;
  if (definition.symbol_view().size() >= model::kSymbolCapacity ||
      model::MakeInstrumentId(model::VenueName(definition.venue),
                              definition.symbol_view(),
                              expected) != base::ErrorCode::kOk ||
      expected != definition.id)
    return invalid("symbol", "invalid instrument identity");
  if (definition.price_decimals > model::kMaxPriceDecimals)
    return invalid("price_decimals", "must be from 0 to 6");
  if (definition.type != model::InstrumentType::kEquity &&
      definition.type != model::InstrumentType::kFutures)
    return invalid("instrument_type", "must be equity or futures");
  const bool futures = definition.type == model::InstrumentType::kFutures;
  if (futures != model::IsFuturesVenue(definition.venue))
    return invalid("instrument_type", "instrument type does not match venue");
  if (definition.price_tick <= 0)
    return invalid("price_tick", "must be positive");
  if (definition.quantity_step <= 0)
    return invalid("quantity_step", "must be positive");
  if (futures) {
    if (definition.contract_multiplier <= 0)
      return invalid("contract_multiplier", "must be positive");
    if (definition.margin_rate_ppm <= 0 ||
        definition.margin_rate_ppm > model::kMarginRateScale)
      return invalid("margin_rate_ppm", "must be from 1 to 1000000");
    if (definition.open_fee_per_lot < 0)
      return invalid("open_fee_per_lot", "must be nonnegative");
    if (definition.close_today_fee_per_lot < 0)
      return invalid("close_today_fee_per_lot", "must be nonnegative");
    if (definition.close_yesterday_fee_per_lot < 0)
      return invalid("close_yesterday_fee_per_lot", "must be nonnegative");
  } else if (definition.contract_multiplier != 1 ||
             definition.margin_rate_ppm != 0 ||
             definition.open_fee_per_lot != 0 ||
             definition.close_today_fee_per_lot != 0 ||
             definition.close_yesterday_fee_per_lot != 0) {
    return invalid("instrument_type",
                   "equity cannot use futures specifications");
  }
  error.Clear();
  return {};
}

base::Status InstrumentCatalogLoader::Load(const char* path) noexcept {
  size_ = 0;
  error_.Clear();
  JsonConfigDocument document;
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
      return Invalid("instruments", "duplicate venue and symbol", error_);
  }
  size_ = count;
  return {};
}

bool InstrumentCatalogLoader::Find(
    model::InstrumentId id, model::InstrumentDefinition& out) const noexcept {
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

base::Status ResolveInstrumentCatalogPath(
    const char* config_path, std::string_view reference, std::span<char> out,
    strategy::ConfigError& error) noexcept {
  error.Clear();
  if (config_path == nullptr || config_path[0] == '\0' ||
      ::strnlen(config_path, kMaxInstrumentCatalogPathLength + 1) >
          kMaxInstrumentCatalogPathLength ||
      reference.empty() || reference.size() > kMaxInstrumentCatalogPathLength ||
      reference.find('\0') != std::string_view::npos) {
    return Invalid("instrument_catalog",
                   "paths must be nonempty, bounded text without NUL", error);
  }
  auto resolved = std::filesystem::path(reference);
  if (resolved.is_relative()) {
    std::error_code filesystem_error;
    resolved = std::filesystem::absolute(
        std::filesystem::path(config_path).parent_path() / resolved,
        filesystem_error);
    if (filesystem_error) {
      error.Set("instrument_catalog", "cannot resolve relative catalog path");
      return {base::ErrorCode::kIoError, filesystem_error};
    }
  }
  const auto& path = resolved.native();
  if (path.size() > kMaxInstrumentCatalogPathLength ||
      path.size() >= out.size())
    return Invalid("instrument_catalog",
                   "resolved path is too long for the catalog path buffer",
                   error);
  std::memcpy(out.data(), path.c_str(), path.size() + 1);
  return {};
}

}  // namespace yquant::strategyd
