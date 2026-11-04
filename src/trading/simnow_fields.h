#ifndef YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_
#define YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace yquant::trading::internal {

// Copies a nonempty field into a zero-initialized output array. Returns false
// without changing out if input is empty, contains a null byte, or does not fit
// with room for a terminator. Relies on the existing zero bytes for termination.
template <std::size_t N>
bool Copy(char (&out)[N], std::string_view in) {
  if (in.empty() || in.size() >= N || in.find('\0') != in.npos) return false;
  std::copy(in.begin(), in.end(), out);
  return true;
}

// Returns the field up to its first null byte, or the full array if none exists.
template <std::size_t N>
std::string_view Field(const char (&value)[N]) {
  return {value, static_cast<std::size_t>(
                     std::find(value, value + N, '\0') - value)};
}

// Removes leading and trailing space padding from a CTP identifier.
template <std::size_t N>
std::string NativeId(const char (&raw)[N]) {
  const auto value = Field(raw);
  const auto first = value.find_first_not_of(' ');
  if (first == value.npos) return {};
  return std::string(value.substr(first, value.find_last_not_of(' ') - first + 1));
}

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_
