#include <cstdint>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyPlugin;

ErrorCode ParseError(simdjson::error_code result, std::string_view path,
                     ConfigError* error) noexcept {
  error->Set(path, simdjson::error_message(result));
  return result == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                      : ErrorCode::kInvalidArgument;
}

class ParameterizedConfig final : public StrategyConfig {
 public:
  std::int64_t quote_qty() const noexcept { return quote_qty_; }
  std::int64_t max_position() const noexcept { return max_position_; }

 protected:
  ErrorCode LoadParameters(std::string_view parameters_json,
                           ConfigError* error) noexcept override {
    // The parser and padded input are initialization-only storage. No JSON
    // views escape this call, including decoded object keys.
    simdjson::padded_string input(parameters_json);
    if (input.data() == nullptr) {
      error->Set("", "could not allocate parameter input");
      return ErrorCode::kResourceExhausted;
    }
    simdjson::dom::parser parser;
    if (const auto result = parser.allocate(input.size());
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::element root;
    if (const auto result = parser.parse(input).get(root);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::object object;
    if (const auto result = root.get_object().get(object);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }

    std::int64_t quote_qty = 0;
    std::int64_t max_position = 0;
    bool has_quote_qty = false;
    bool has_max_position = false;
    for (const auto field : object) {
      // DOM keys are decoded, so "quote_qty" and "quote_\u0071ty" name the
      // same field. Do not coerce strings or floating-point values to integers.
      if (field.key == "quote_qty") {
        if (has_quote_qty) {
          error->Set(field.key, "duplicate parameter");
          return ErrorCode::kInvalidArgument;
        }
        if (const auto result = field.value.get_int64().get(quote_qty);
            result != simdjson::SUCCESS) {
          return ParseError(result, field.key, error);
        }
        has_quote_qty = true;
      } else if (field.key == "max_position") {
        if (has_max_position) {
          error->Set(field.key, "duplicate parameter");
          return ErrorCode::kInvalidArgument;
        }
        if (const auto result = field.value.get_int64().get(max_position);
            result != simdjson::SUCCESS) {
          return ParseError(result, field.key, error);
        }
        has_max_position = true;
      } else {
        error->Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (!has_quote_qty) {
      error->Set("quote_qty", "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    if (!has_max_position) {
      error->Set("max_position", "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    if (quote_qty <= 0) {
      error->Set("quote_qty", "must be greater than zero");
      return ErrorCode::kInvalidArgument;
    }
    if (max_position < quote_qty) {
      error->Set("max_position", "must be at least quote_qty");
      return ErrorCode::kInvalidArgument;
    }

    // Publish the typed configuration only after all validation succeeds.
    quote_qty_ = quote_qty;
    max_position_ = max_position;
    return ErrorCode::kOk;
  }

 private:
  std::int64_t quote_qty_ = 0;
  std::int64_t max_position_ = 0;
};

class ParameterizedStrategy final : public Strategy {
 public:
  ParameterizedConfig& config() noexcept override { return config_; }

  ErrorCode OnStart() noexcept override {
    if (state_ != State::kIdle || !config_.ready()) {
      return ErrorCode::kInvalidState;
    }
    // Strategy logic reads typed fields directly, without parsing JSON again.
    if (config_.quote_qty() <= 0 ||
        config_.max_position() < config_.quote_qty()) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kRunning;
    return ErrorCode::kOk;
  }

  ErrorCode OnStop() noexcept override {
    if (state_ != State::kRunning) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kStopped;
    return ErrorCode::kOk;
  }

 private:
  enum class State { kIdle, kRunning, kStopped };

  ParameterizedConfig config_;
  State state_ = State::kIdle;
};

ErrorCode CreateStrategy(Strategy** output) noexcept {
  if (output == nullptr) {
    return ErrorCode::kInvalidArgument;
  }
  *output = nullptr;
  auto* strategy = new (std::nothrow) ParameterizedStrategy;
  if (strategy == nullptr) {
    return ErrorCode::kResourceExhausted;
  }
  *output = strategy;
  return ErrorCode::kOk;
}

void DestroyStrategy(Strategy* strategy) noexcept { delete strategy; }

const StrategyPlugin kPlugin = {
    yquant::strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(StrategyPlugin)), CreateStrategy,
    DestroyStrategy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
