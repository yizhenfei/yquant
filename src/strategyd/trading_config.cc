#include "strategyd/trading_config.h"

#include <simdjson.h>

#include <array>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

#include "strategyd/json_config_document.h"

namespace yquant::strategyd {
namespace {

// Owns the schema failure text without retaining input views.
base::Status Invalid(std::string_view field, std::string_view message,
                     strategy::ConfigError& error) noexcept {
  error.Set(field, message);
  return {base::ErrorCode::kInvalidArgument, {}};
}

// Converts checked parser errors to project status and copied diagnostics.
base::Status JsonError(simdjson::error_code code, std::string_view field,
                       strategy::ConfigError& error) noexcept {
  error.Set(field, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Builds a bounded path without retaining key text from the parser.
struct PositionFieldPath {
  char text[256] = {};
  bool truncated = false;

  PositionFieldPath(std::size_t index, std::string_view key) noexcept {
    const int size =
        key.empty()
            ? std::snprintf(text, sizeof(text), "initial_positions[%zu]", index)
            : std::snprintf(text, sizeof(text), "initial_positions[%zu].%.*s",
                            index, static_cast<int>(key.size()), key.data());
    truncated = size < 0 || static_cast<std::size_t>(size) >= sizeof(text);
  }
};

// Parses one owned position. reserved_qty remains zero because bootstrap JSON
// never describes local reservations; the host validates dictionary membership.
base::Status ParsePosition(simdjson::ondemand::value value, std::size_t index,
                           model::PositionSnapshot& out,
                           strategy::ConfigError& error) noexcept {
  const PositionFieldPath object_path(index, {});
  simdjson::ondemand::object object;
  auto json_error = value.get_object().get(object);
  if (json_error) return JsonError(json_error, object_path.text, error);
  constexpr std::array<std::string_view, 4> kFields = {
      "venue", "symbol", "total_qty", "available_qty"};
  std::uint32_t seen = 0;
  std::string_view venue;
  std::string_view symbol;
  model::PositionSnapshot parsed;
  for (auto field_result : object) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, object_path.text, error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, object_path.text, error);
    const PositionFieldPath path(index, key);
    std::size_t position = 0;
    while (position < kFields.size() && key != kFields[position]) ++position;
    if (position == kFields.size()) {
      const auto status = Invalid(path.text, "unknown position field", error);
      error.truncated |= path.truncated;
      return status;
    }
    seen |= 1U << position;
    if (position < 2) {
      std::string_view decoded;
      json_error = field.value().get_string().get(decoded);
      if (json_error) return JsonError(json_error, path.text, error);
      if (position == 0)
        venue = decoded;
      else
        symbol = decoded;
    } else {
      std::int64_t quantity;
      json_error = field.value().get_int64().get(quantity);
      if (json_error) return JsonError(json_error, path.text, error);
      if (position == 2)
        parsed.total_qty = quantity;
      else
        parsed.available_qty = quantity;
    }
  }
  for (std::size_t field = 0; field < kFields.size(); ++field) {
    if ((seen & (1U << field)) == 0) {
      return Invalid(PositionFieldPath(index, kFields[field]).text,
                     "required field is missing", error);
    }
  }
  const auto id_status =
      model::MakeInstrumentId(venue, symbol, parsed.instrument_id);
  if (id_status != base::ErrorCode::kOk) {
    const auto key = venue != "SSE" && venue != "SZSE" && venue != "BSE"
                         ? "venue"
                         : "symbol";
    return Invalid(PositionFieldPath(index, key).text,
                   "requires a supported venue and six ASCII symbol digits",
                   error);
  }
  out = parsed;
  return {};
}

// Consumes the complete bounded array into temporary storage. The caller does
// semantic validation before publishing the resulting configuration.
base::Status ParsePositions(simdjson::ondemand::value value,
                            TradingAccountSnapshot& account,
                            strategy::ConfigError& error) noexcept {
  simdjson::ondemand::array positions;
  auto json_error = value.get_array().get(positions);
  if (json_error) return JsonError(json_error, "initial_positions", error);
  for (auto item_result : positions) {
    if (account.position_count == account.positions.size()) {
      return Invalid("initial_positions", "at most 64 positions are allowed",
                     error);
    }
    simdjson::ondemand::value item;
    json_error = std::move(item_result).get(item);
    if (json_error) return JsonError(json_error, "initial_positions", error);
    const auto status =
        ParsePosition(item, account.position_count,
                      account.positions[account.position_count], error);
    if (!status.ok()) return status;
    ++account.position_count;
  }
  return {};
}

// Owns a path for diagnostics from the separate futures bootstrap schema.
struct FuturesFieldPath {
  char text[256] = {};
  bool truncated = false;
  FuturesFieldPath(std::size_t index, std::string_view key) noexcept {
    const auto size =
        key.empty()
            ? std::snprintf(text, sizeof(text),
                            "initial_futures_positions[%zu]", index)
            : std::snprintf(text, sizeof(text),
                            "initial_futures_positions[%zu].%.*s", index,
                            static_cast<int>(key.size()), key.data());
    truncated = size < 0 || static_cast<std::size_t>(size) >= sizeof(text);
  }
};

// Reads one FIFO bootstrap bucket; validation runs on the complete typed
// result.
base::Status ParseBucket(simdjson::ondemand::value value, std::string_view path,
                         FuturesInitialBucket& out,
                         strategy::ConfigError& error) noexcept {
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, path, error);
  std::uint32_t seen = 0;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, path, error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, path, error);
    char full_path[256];
    const auto size = std::snprintf(full_path, sizeof(full_path), "%.*s.%.*s",
                                    static_cast<int>(path.size()), path.data(),
                                    static_cast<int>(key.size()), key.data());
    if (key != "quantity" && key != "open_price") {
      const auto status = Invalid(full_path, "unknown bucket field", error);
      error.truncated |=
          size < 0 || static_cast<std::size_t>(size) >= sizeof(full_path);
      return status;
    }
    std::int64_t parsed;
    code = field.value().get_int64().get(parsed);
    if (code) return JsonError(code, full_path, error);
    if (key == "quantity") {
      seen |= 1;
      out.quantity = parsed;
    } else {
      seen |= 2;
      out.open_price = parsed;
    }
  }
  if (seen != 3)
    return Invalid(path, "quantity and open_price are required", error);
  return {};
}

// Futures inventory is kept separate from equity quantity/availability fields.
base::Status ParseFuturesPosition(simdjson::ondemand::value value,
                                  std::size_t index,
                                  FuturesInitialPosition& out,
                                  strategy::ConfigError& error) noexcept {
  constexpr std::array<std::string_view, 7> kFields = {
      "venue",          "symbol",      "reference_price", "long_today",
      "long_yesterday", "short_today", "short_yesterday"};
  const FuturesFieldPath object_path(index, {});
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, object_path.text, error);
  std::uint32_t seen = 0;
  std::string_view venue, symbol;
  for (auto item : object) {
    simdjson::ondemand::field field;
    code = std::move(item).get(field);
    if (code) return JsonError(code, object_path.text, error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, object_path.text, error);
    const FuturesFieldPath path(index, key);
    std::size_t offset = 0;
    while (offset < kFields.size() && key != kFields[offset]) ++offset;
    if (offset == kFields.size()) {
      const auto status =
          Invalid(path.text, "unknown futures position field", error);
      error.truncated |= path.truncated;
      return status;
    }
    seen |= 1U << offset;
    if (offset < 2) {
      code = field.value().get_string().get(offset == 0 ? venue : symbol);
    } else if (offset == 2) {
      code = field.value().get_int64().get(out.reference_price);
    } else {
      FuturesInitialBucket* buckets[] = {&out.long_today, &out.long_yesterday,
                                         &out.short_today,
                                         &out.short_yesterday};
      const auto status =
          ParseBucket(field.value(), path.text, *buckets[offset - 3], error);
      if (!status.ok()) return status;
    }
    if (code) return JsonError(code, path.text, error);
  }
  for (std::size_t offset = 0; offset < kFields.size(); ++offset)
    if ((seen & (1U << offset)) == 0)
      return Invalid(FuturesFieldPath(index, kFields[offset]).text,
                     "required field is missing", error);
  if (model::MakeInstrumentId(venue, symbol, out.instrument_id) !=
          base::ErrorCode::kOk ||
      !model::IsFuturesVenue(
          static_cast<model::Venue>(out.instrument_id.value >> 32)))
    return Invalid(FuturesFieldPath(index, "symbol").text,
                   "requires a supported futures venue and contract symbol",
                   error);
  return {};
}

// Consumes all bootstrap positions before publishing any configuration.
base::Status ParseFuturesPositions(simdjson::ondemand::value value,
                                   TradingAccountSnapshot& account,
                                   strategy::ConfigError& error) noexcept {
  simdjson::ondemand::array array;
  auto code = value.get_array().get(array);
  if (code) return JsonError(code, "initial_futures_positions", error);
  for (auto item : array) {
    if (account.futures_position_count == account.futures_positions.size())
      return Invalid("initial_futures_positions",
                     "at most 64 positions are allowed", error);
    simdjson::ondemand::value parsed;
    code = std::move(item).get(parsed);
    if (code) return JsonError(code, "initial_futures_positions", error);
    const auto status = ParseFuturesPosition(
        parsed, account.futures_position_count,
        account.futures_positions[account.futures_position_count], error);
    if (!status.ok()) return status;
    ++account.futures_position_count;
  }
  return {};
}

// Parses exactly one strict object after generic syntax/UTF-8/duplicate-key
// validation. Writes only temporary typed values; no parser views escape.
base::Status Parse(const JsonConfigDocument& source, TradingConfig& config,
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
  constexpr std::array<std::string_view, 7> kFields = {
      "adapter",
      "mode",
      "report_interval_ms",
      "shutdown_timeout_ms",
      "initial_cash",
      "initial_positions",
      "initial_futures_positions"};
  std::uint32_t seen = 0;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, "$", error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, "$", error);
    std::size_t position = 0;
    while (position < kFields.size() && key != kFields[position]) ++position;
    if (position == kFields.size()) return Invalid(key, "unknown field", error);
    seen |= 1U << position;
    if (position < 2) {
      std::string_view value;
      json_error = field.value().get_string().get(value);
      if (json_error) return JsonError(json_error, key, error);
      if (position == 0) {
        if (value == "mock") {
          config.adapter = TradingAdapterType::kMock;
        } else if (value == "simulated_futures") {
          config.adapter = TradingAdapterType::kSimulatedFutures;
          config.initial_account.type = model::AccountType::kFutures;
        } else {
          return Invalid(key, "must be mock or simulated_futures", error);
        }
      } else if (value == "fill") {
        config.mode = MockTradingMode::kFill;
      } else if (value == "working") {
        config.mode = MockTradingMode::kWorking;
      } else if (value == "reject") {
        config.mode = MockTradingMode::kReject;
      } else {
        return Invalid(key, "must be fill, working or reject", error);
      }
    } else if (position < 4) {
      std::uint64_t milliseconds;
      json_error = field.value().get_uint64().get(milliseconds);
      if (json_error) return JsonError(json_error, key, error);
      if (milliseconds < 1 || milliseconds > 60000) {
        return Invalid(key, "must be an integer from 1 to 60000", error);
      }
      const auto duration = milliseconds * 1000000;
      if (position == 2)
        config.report_interval_ns = duration;
      else
        config.shutdown_timeout_ns = duration;
    } else if (position == 4) {
      json_error =
          field.value().get_int64().get(config.initial_account.cash_balance);
      if (json_error) return JsonError(json_error, key, error);
    } else {
      const auto status =
          position == 5
              ? ParsePositions(field.value(), config.initial_account, error)
              : ParseFuturesPositions(field.value(), config.initial_account,
                                      error);
      if (!status.ok()) return status;
    }
  }
  const bool futures = config.adapter == TradingAdapterType::kSimulatedFutures;
  const std::uint32_t required = futures ? 0b1011101U : 0b0111111U;
  if (futures && (seen & ((1U << 1) | (1U << 5))))
    return Invalid((seen & (1U << 1)) ? "mode" : "initial_positions",
                   "field is not used by simulated_futures", error);
  if (!futures && (seen & (1U << 6)))
    return Invalid("initial_futures_positions", "field is not used by mock",
                   error);
  for (std::size_t position = 0; position < kFields.size(); ++position) {
    if ((required & (1U << position)) == 0) continue;
    if ((seen & (1U << position)) == 0) {
      return Invalid(kFields[position], "required field is missing", error);
    }
  }
  return {};
}

}  // namespace

base::Status ValidateTradingConfig(const TradingConfig& config,
                                   strategy::ConfigError& error) noexcept {
  error.Clear();
  if (config.adapter != TradingAdapterType::kMock &&
      config.adapter != TradingAdapterType::kSimulatedFutures)
    return Invalid("adapter", "must be mock or simulated_futures", error);
  const bool futures = config.adapter == TradingAdapterType::kSimulatedFutures;
  if (config.initial_account.type !=
      (futures ? model::AccountType::kFutures : model::AccountType::kCash))
    return Invalid("adapter", "adapter and account type must agree", error);
  if (config.mode != MockTradingMode::kFill &&
      config.mode != MockTradingMode::kWorking &&
      config.mode != MockTradingMode::kReject) {
    return Invalid("mode", "must be fill, working or reject", error);
  }
  constexpr std::uint64_t kMinimum = 1000000;
  constexpr std::uint64_t kMaximum = 60000000000ULL;
  if (config.report_interval_ns < kMinimum ||
      config.report_interval_ns > kMaximum) {
    return Invalid("report_interval_ms", "must be from 1 to 60000 milliseconds",
                   error);
  }
  if (config.shutdown_timeout_ns < kMinimum ||
      config.shutdown_timeout_ns > kMaximum) {
    return Invalid("shutdown_timeout_ms",
                   "must be from 1 to 60000 milliseconds", error);
  }
  const auto& account = config.initial_account;
  if (account.cash_balance < 0) {
    return Invalid("initial_cash",
                   "must be a nonnegative int64 microyuan value", error);
  }
  if ((futures && account.position_count != 0) ||
      (!futures && account.futures_position_count != 0))
    return Invalid(futures ? "initial_positions" : "initial_futures_positions",
                   "account types cannot be mixed", error);
  if (account.futures_position_count > account.futures_positions.size())
    return Invalid("initial_futures_positions",
                   "at most 64 positions are allowed", error);
  for (std::size_t index = 0; index < account.futures_position_count; ++index) {
    const auto& position = account.futures_positions[index];
    if (!model::IsValidInstrumentId(position.instrument_id) ||
        !model::IsFuturesVenue(
            static_cast<model::Venue>(position.instrument_id.value >> 32)))
      return Invalid(FuturesFieldPath(index, "symbol").text,
                     "invalid futures instrument identity", error);
    if (position.reference_price <= 0)
      return Invalid(FuturesFieldPath(index, "reference_price").text,
                     "must be positive", error);
    const FuturesInitialBucket* buckets[] = {
        &position.long_today, &position.long_yesterday, &position.short_today,
        &position.short_yesterday};
    constexpr std::string_view kNames[] = {"long_today", "long_yesterday",
                                           "short_today", "short_yesterday"};
    std::int64_t gross = 0;
    for (std::size_t bucket = 0; bucket < 4; ++bucket) {
      const auto& entry = *buckets[bucket];
      if (entry.quantity < 0 || entry.open_price < 0 ||
          ((entry.quantity == 0) != (entry.open_price == 0)))
        return Invalid(FuturesFieldPath(index, kNames[bucket]).text,
                       "quantity and open_price must both be zero or positive",
                       error);
      if (entry.quantity > std::numeric_limits<std::int64_t>::max() - gross)
        return Invalid(FuturesFieldPath(index, kNames[bucket]).text,
                       "gross quantity exceeds int64", error);
      gross += entry.quantity;
    }
    for (std::size_t previous = 0; previous < index; ++previous)
      if (account.futures_positions[previous].instrument_id ==
          position.instrument_id)
        return Invalid(FuturesFieldPath(index, "symbol").text,
                       "duplicate initial position", error);
  }
  if (account.position_count > account.positions.size()) {
    return Invalid("initial_positions", "at most 64 positions are allowed",
                   error);
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    // Stable IDs encode one of the three supported venues and six digits.
    // Dictionary membership is checked by the host during account Configure.
    const auto venue = position.instrument_id.value >> 32;
    const auto symbol = position.instrument_id.value & UINT32_MAX;
    if (venue < 1 || venue > 3 || symbol > 999999) {
      return Invalid(PositionFieldPath(index, "symbol").text,
                     "invalid stable instrument identity", error);
    }
    if (position.total_qty < 0) {
      return Invalid(PositionFieldPath(index, "total_qty").text,
                     "must be nonnegative", error);
    }
    if (position.available_qty < 0 ||
        position.available_qty > position.total_qty) {
      return Invalid(PositionFieldPath(index, "available_qty").text,
                     "must be nonnegative and no greater than total_qty",
                     error);
    }
    if (position.reserved_qty != 0) {
      return Invalid(PositionFieldPath(index, "reserved_qty").text,
                     "initial reservations must be zero", error);
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id) {
        return Invalid(PositionFieldPath(index, "symbol").text,
                       "duplicate initial position", error);
      }
    }
  }
  return {};
}

base::Status TradingConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  JsonConfigDocument source;
  auto status = source.Load(path);
  if (!status.ok()) {
    error_ = source.error();
    return status;
  }
  TradingConfig parsed;
  status = Parse(source, parsed, error_);
  if (status.ok()) status = ValidateTradingConfig(parsed, error_);
  if (status.ok()) config_ = parsed;
  return status;
}

}  // namespace yquant::strategyd
