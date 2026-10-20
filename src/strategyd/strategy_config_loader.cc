#include "strategyd/strategy_config_loader.h"

#include <simdjson.h>

#include <utility>

namespace yquant::strategyd {
namespace {

// Copies parser failures before its temporary storage is destroyed.
base::Status JsonError(simdjson::error_code code, std::string_view path,
                       strategy::ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Parses common fields from already fully validated source. parameters borrows
// the source unchanged; the caller owns it until configuration initialization.
base::Status Parse(const JsonConfigDocument& source, strategy::StrategyId& id,
                   std::string_view& parameters,
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
  bool has_id = false;
  bool has_parameters = false;
  for (auto field_result : root) {
    simdjson::ondemand::field field;
    json_error = std::move(field_result).get(field);
    if (json_error) return JsonError(json_error, "$", error);
    std::string_view key;
    json_error = field.unescaped_key().get(key);
    if (json_error) return JsonError(json_error, "$", error);
    if (key == "strategy_id") {
      std::string_view value;
      json_error = field.value().get_string().get(value);
      if (json_error) return JsonError(json_error, key, error);
      const auto code = id.Assign(value);
      if (code != base::ErrorCode::kOk) {
        error.Set(key, "must contain 1 to 64 UTF-8 bytes without NUL");
        return {code, {}};
      }
      has_id = true;
    } else if (key == "parameters") {
      simdjson::ondemand::object object;
      json_error = field.value().get_object().get(object);
      if (json_error) return JsonError(json_error, key, error);
      json_error = object.raw_json().get(parameters);
      if (json_error) return JsonError(json_error, key, error);
      while (!parameters.empty()) {
        const char last = parameters.back();
        if (last != ' ' && last != '\t' && last != '\n' && last != '\r') break;
        parameters.remove_suffix(1);
      }
      has_parameters = true;
    } else {
      error.Set(key, "unknown root field");
      return {base::ErrorCode::kInvalidArgument, {}};
    }
  }
  if (!has_id || !has_parameters) {
    error.Set(!has_id ? "strategy_id" : "parameters",
              "required field is missing");
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  return {};
}

}  // namespace

StrategyConfigLoader::~StrategyConfigLoader() noexcept { Reset(); }

void StrategyConfigLoader::Reset() noexcept {
  document_.Reset();
  strategy_id_ = {};
  parameters_json_ = {};
}

base::Status StrategyConfigLoader::Load(const char* path) noexcept {
  Reset();
  error_.Clear();
  base::Status result = document_.Load(path);
  if (!result.ok()) {
    error_ = document_.error();
    return result;
  }
  result = Parse(document_, strategy_id_, parameters_json_, error_);
  if (!result.ok()) Reset();
  return result;
}

}  // namespace yquant::strategyd
