#ifndef YQUANT_MODEL_CURRENCY_H_
#define YQUANT_MODEL_CURRENCY_H_

#include <algorithm>
#include <array>
#include <string_view>

namespace yquant::model {

// Represents a currency using a three-letter uppercase code.
// An unspecified currency has valid() == false and an empty code().
class Currency {
 public:
  constexpr Currency() = default;
  // Copies a complete three-letter code. Malformed input produces an
  // unspecified currency; check valid() for the result.
  constexpr explicit Currency(std::string_view code) {
    if (IsValidCode(code)) {
      code_ = {code[0], code[1], code[2]};
    }
  }

  constexpr bool valid() const { return code_[0] != '\0'; }
  // Returns a view of the stored code, or an empty view for an unspecified currency.
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
