#include "simnow_order_tag.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

#include "simnow_fields.h"

namespace yquant::trading::internal {
namespace {

// Five printable base-85 digits preserve one 32-bit word. The two SDK
// fields together hold a version prefix and all six words without truncation.
constexpr std::string_view kDigits =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
    ".-:+=^!/*?&<>()[]{}@%$#";
static_assert(kDigits.size() == 85 && sizeof(TThostFtdcBusinessUnitType) == 21 &&
              sizeof(TThostFtdcOrderMemoType) == 13);

}  // namespace

bool WriteOrderTag(const SimNowOrderOwner& owner, CThostFtdcInputOrderField& row) {
  if (!owner.order_id.sequence) return false;
  std::array<char, 32> tag{'Y', '1'};
  std::size_t pos = 2;
  for (auto value : {owner.order_id.caller_service_id, owner.order_id.sequence,
                     owner.strategy_id}) {
    for (int shift : {32, 0}) {
      auto word = static_cast<std::uint32_t>(value >> shift);
      for (int i = 4; i >= 0; --i) {
        tag[pos + i] = kDigits[word % 85];
        word /= 85;
      }
      pos += 5;
    }
  }
  std::copy_n(tag.data(), 20, row.BusinessUnit);
  row.BusinessUnit[20] = 0;
  std::copy_n(tag.data() + 20, 12, row.OrderMemo);
  row.OrderMemo[12] = 0;
  return true;
}

std::optional<SimNowOrderOwner> ReadOrderTag(const CThostFtdcOrderField& row) {
  const auto head = Field(row.BusinessUnit), tail = Field(row.OrderMemo);
  if (head.size() != 20 || !head.starts_with("Y1") || tail.size() != 12) {
    return std::nullopt;
  }
  std::array<std::uint64_t, 3> values{};
  std::size_t pos = 2;
  for (auto& value : values) {
    for (int part = 0; part < 2; ++part) {
      std::uint64_t word = 0;
      for (int i = 0; i < 5; ++i, ++pos) {
        auto digit = kDigits.find(pos < 20 ? head[pos] : tail[pos - 20]);
        if (digit == kDigits.npos) return std::nullopt;
        word = word * 85 + digit;
      }
      if (word > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
      value = (value << 32) | word;
    }
  }
  if (!values[1]) return std::nullopt;
  return SimNowOrderOwner{{values[0], values[1]}, values[2]};
}

}  // namespace yquant::trading::internal
