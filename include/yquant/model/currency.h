#ifndef YQUANT_MODEL_CURRENCY_H_
#define YQUANT_MODEL_CURRENCY_H_

#include <algorithm>
#include <array>
#include <string_view>

namespace yquant::model {

// Stores a currency code as three uppercase letters without allocating memory.
// An unspecified currency has valid() == false and an empty code().
class Currency {
 public:
  constexpr Currency() = default;
  // Copies a complete three-letter code. Malformed input produces an
  // unspecified currency; check valid() for the result. No reference to the
  // input is retained.
  constexpr explicit Currency(std::string_view code) {
    if (IsValidCode(code)) {
      code_ = {code[0], code[1], code[2]};
    }
  }

  constexpr bool valid() const { return code_[0] != '\0'; }
  // Returns the stored code or an empty view for an unspecified currency.
  // The view is valid until this currency is reassigned or destroyed.
  constexpr std::string_view code() const {
    return valid() ? std::string_view(code_.data(), code_.size())
                   : std::string_view{};
  }
  bool operator==(const Currency&) const = default;

 private:
  // Accepts exactly three uppercase letters; longer inputs are never truncated.
  static constexpr bool IsValidCode(std::string_view code) {
    return code.size() == 3 &&
           std::all_of(code.begin(), code.end(), [](char letter) {
             return letter >= 'A' && letter <= 'Z';
           });
  }

  std::array<char, 3> code_{};
};

}  // namespace yquant::model

#endif  // YQUANT_MODEL_CURRENCY_H_
