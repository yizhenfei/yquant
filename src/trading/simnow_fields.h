#ifndef YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_
#define YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace yquant::trading::internal {

template <std::size_t N>
bool Copy(char (&out)[N], std::string_view in) {
  if (in.empty() || in.size() >= N || in.find('\0') != in.npos) return false;
  std::copy(in.begin(), in.end(), out);
  return true;  // Request structures are zero-initialized.
}

template <std::size_t N>
std::string_view Field(const char (&value)[N]) {
  return {value, static_cast<std::size_t>(
                     std::find(value, value + N, '\0') - value)};
}

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_FIELDS_H_
