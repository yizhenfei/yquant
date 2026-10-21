#include "yquant/trading/trading_config.h"

#include <simdjson.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "base/internal/json_config_parser.h"
#include "yquant/base/json_config_document.h"

namespace yquant::trading {
namespace {

using base::InvalidConfig;
using base::internal::JsonError;
using base::internal::ParseConfigObject;

// Builds a bounded path without retaining key text from the parser.
struct PositionFieldPath {
  char text[sizeof(base::ConfigError::field_path)] = {};
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
                           base::ConfigError& error) noexcept {
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
      const auto status =
          InvalidConfig(path.text, "unknown position field", error);
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
      return InvalidConfig(PositionFieldPath(index, kFields[field]).text,
                           "required field is missing", error);
    }
  }
  const auto id_status =
      model::MakeInstrumentId(venue, symbol, parsed.instrument_id);
  if (id_status != base::ErrorCode::kOk) {
    const auto key = venue != "SSE" && venue != "SZSE" && venue != "BSE"
                         ? "venue"
                         : "symbol";
    return InvalidConfig(
        PositionFieldPath(index, key).text,
        "requires a supported venue and six ASCII symbol digits", error);
  }
  out = parsed;
  return {};
}

// Consumes the complete bounded array into temporary storage. The caller does
// semantic validation before publishing the resulting configuration.
base::Status ParsePositions(simdjson::ondemand::value value,
                            TradingAccountSnapshot& account,
                            base::ConfigError& error) noexcept {
  simdjson::ondemand::array positions;
  auto json_error = value.get_array().get(positions);
  if (json_error) return JsonError(json_error, "initial_positions", error);
  for (auto item_result : positions) {
    if (account.position_count == account.positions.size()) {
      return InvalidConfig("initial_positions",
                           "at most 64 positions are allowed", error);
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

// Parses exactly one strict object after generic syntax/UTF-8/duplicate-key
// validation. Writes only temporary typed values; no parser views escape.
base::Status Parse(const base::JsonConfigDocument& source,
                   TradingConfig& config, base::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  simdjson::ondemand::document document;
  simdjson::ondemand::object root;
  const auto parsed = ParseConfigObject(source, parser, document, root, error);
  if (!parsed.ok()) return parsed;
  simdjson::error_code json_error;
  constexpr std::array<std::string_view, 6> kFields = {"adapter",
                                                       "mode",
                                                       "report_interval_ms",
                                                       "shutdown_timeout_ms",
                                                       "initial_cash",
                                                       "initial_positions"};
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
    if (position == kFields.size())
      return InvalidConfig(key, "unknown field", error);
    seen |= 1U << position;
    if (position == 0 || position == 1) {
      std::string_view value;
      json_error = field.value().get_string().get(value);
      if (json_error) return JsonError(json_error, key, error);
      if (position == 0) {
        if (value == "mock")
          config.adapter = TradingAdapterKind::kMock;
        else
          return InvalidConfig(key, "must be mock", error);
      } else if (position == 1) {
        if (value == "fill")
          config.mode = MockTradingMode::kFill;
        else if (value == "working")
          config.mode = MockTradingMode::kWorking;
        else if (value == "reject")
          config.mode = MockTradingMode::kReject;
        else
          return InvalidConfig(key, "must be fill, working or reject", error);
      }
    } else if (position == 2 || position == 3) {
      std::uint64_t milliseconds;
      json_error = field.value().get_uint64().get(milliseconds);
      if (json_error) return JsonError(json_error, key, error);
      if (milliseconds < kMinimumTradingDurationMs ||
          milliseconds > kMaximumTradingDurationMs)
        return InvalidConfig(key, "must be an integer from 1 to 60000", error);
      const auto duration = milliseconds * kTradingNanosecondsPerMillisecond;
      if (position == 2)
        config.report_interval_ns = duration;
      else
        config.shutdown_timeout_ns = duration;
    } else if (position == 4) {
      json_error =
          field.value().get_int64().get(config.initial_account.cash_balance);
      if (json_error) return JsonError(json_error, key, error);
    } else if (position == 5) {
      const auto status =
          ParsePositions(field.value(), config.initial_account, error);
      if (!status.ok()) return status;
    }
  }
  for (std::size_t position = 0; position < kFields.size(); ++position) {
    if ((seen & (1U << position)) == 0)
      return InvalidConfig(kFields[position], "required field is missing",
                           error);
  }
  return {};
}

}  // namespace

base::Status ValidateTradingConfig(const TradingConfig& config,
                                   base::ConfigError& error) noexcept {
  error.Clear();
  constexpr std::uint64_t kMinimum =
      kMinimumTradingDurationMs * kTradingNanosecondsPerMillisecond;
  constexpr std::uint64_t kMaximum =
      kMaximumTradingDurationMs * kTradingNanosecondsPerMillisecond;
  if (config.shutdown_timeout_ns < kMinimum ||
      config.shutdown_timeout_ns > kMaximum)
    return InvalidConfig("shutdown_timeout_ms",
                         "must be from 1 to 60000 milliseconds", error);
  if (config.adapter != TradingAdapterKind::kMock)
    return InvalidConfig("adapter", "must be mock", error);
  if (config.mode != MockTradingMode::kFill &&
      config.mode != MockTradingMode::kWorking &&
      config.mode != MockTradingMode::kReject)
    return InvalidConfig("mode", "must be fill, working or reject", error);
  if (config.report_interval_ns < kMinimum ||
      config.report_interval_ns > kMaximum)
    return InvalidConfig("report_interval_ms",
                         "must be from 1 to 60000 milliseconds", error);
  const auto& account = config.initial_account;
  if (account.cash_balance < 0) {
    return InvalidConfig("initial_cash",
                         "must be a nonnegative int64 microyuan value", error);
  }
  if (account.position_count > account.positions.size()) {
    return InvalidConfig("initial_positions",
                         "at most 64 positions are allowed", error);
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    // Stable IDs encode one of the three supported venues and six digits.
    // Dictionary membership is checked by the host during account Configure.
    const auto venue = position.instrument_id.value >> 32;
    const auto symbol = position.instrument_id.value & UINT32_MAX;
    if (venue < 1 || venue > 3 || symbol > 999999) {
      return InvalidConfig(PositionFieldPath(index, "symbol").text,
                           "invalid stable instrument identity", error);
    }
    if (position.total_qty < 0) {
      return InvalidConfig(PositionFieldPath(index, "total_qty").text,
                           "must be nonnegative", error);
    }
    if (position.available_qty < 0 ||
        position.available_qty > position.total_qty) {
      return InvalidConfig(PositionFieldPath(index, "available_qty").text,
                           "must be nonnegative and no greater than total_qty",
                           error);
    }
    if (position.reserved_qty != 0) {
      return InvalidConfig(PositionFieldPath(index, "reserved_qty").text,
                           "initial reservations must be zero", error);
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id) {
        return InvalidConfig(PositionFieldPath(index, "symbol").text,
                             "duplicate initial position", error);
      }
    }
  }
  return {};
}

base::Status TradingConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  base::JsonConfigDocument source;
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

}  // namespace yquant::trading
