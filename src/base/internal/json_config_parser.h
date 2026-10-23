#ifndef YQUANT_BASE_INTERNAL_JSON_CONFIG_PARSER_H_
#define YQUANT_BASE_INTERNAL_JSON_CONFIG_PARSER_H_

#include <stddef.h>

#include <cstring>
#include <span>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/config_error.h"
#include "yquant/base/json_config_document.h"

namespace yquant::base::internal {

// Copies parser diagnostics; allocation errors return kResourceExhausted and
// all other parser errors return kInvalidArgument. Retains no parser views.
[[nodiscard]] inline Status JsonError(simdjson::error_code code,
                                      std::string_view field,
                                      ConfigError& error) noexcept {
  error.Set(field, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                     : ErrorCode::kInvalidArgument,
          {}};
}

// Decodes nonempty text without NUL into out, including its NUL terminator.
// On failure out is unchanged and error owns the diagnostic. Retains no views.
[[nodiscard]] inline Status ReadConfigText(
    simdjson::ondemand::value value, std::span<char> out,
    std::string_view field, ConfigError& error,
    std::string_view invalid_message =
        "empty, overlong or NUL-containing string") noexcept {
  std::string_view text;
  const simdjson::error_code json_error = value.get_string().get(text);
  if (json_error) {
    return JsonError(json_error, field, error);
  }
  if (text.empty() || text.size() >= out.size() ||
      text.find('\0') != std::string_view::npos) {
    return InvalidConfig(field, invalid_message, error);
  }
  std::memmove(out.data(), text.data(), text.size());
  out[text.size()] = '\0';
  return {};
}

// Begins a traversal of padded source, during full validation or later schema
// parsing. source and parser must outlive document/root and all extracted
// views. On failure error owns the diagnostic; do not consume document/root.
[[nodiscard]] inline Status ParseConfigObject(
    const JsonConfigDocument& source, simdjson::ondemand::parser& parser,
    simdjson::ondemand::document& document, simdjson::ondemand::object& root,
    ConfigError& error) noexcept {
  // Leave parser traversal room beyond the explicitly checked container depth.
  constexpr size_t kParserDepth = kMaxConfigDepth + 4;
  simdjson::error_code json_error =
      parser.allocate(source.source().size(), kParserDepth);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  json_error = parser.iterate(source.source(), source.capacity()).get(document);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  json_error = document.get_object().get(root);
  if (json_error) {
    return JsonError(json_error, "$", error);
  }
  return {};
}

}  // namespace yquant::base::internal

#endif  // YQUANT_BASE_INTERNAL_JSON_CONFIG_PARSER_H_
