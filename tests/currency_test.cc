#include "yquant/model/currency.h"

#include <string>
#include <string_view>

#include "gtest/gtest.h"

namespace yquant::model {
namespace {

TEST(CurrencyTest, DefaultValueIsUnspecified) {
  const Currency currency;
  EXPECT_FALSE(currency.valid());
  EXPECT_TRUE(currency.code().empty());
}

TEST(CurrencyTest, PreservesCompleteUppercaseCodes) {
  for (std::string_view code : {"CNY", "USD", "EUR", "JPY", "ABC"}) {
    const Currency currency(code);
    EXPECT_TRUE(currency.valid());
    EXPECT_EQ(currency.code(), code);
  }
}

TEST(CurrencyTest, RejectsMalformedInputWithoutTruncatingIt) {
  for (std::string_view code : {"", "CN", "USDT", "cNY", "CnY", "CNy",
                                "1NY", "C2Y", "CN3", " CN", "CNY "}) {
    const Currency currency(code);
    EXPECT_FALSE(currency.valid()) << code;
    EXPECT_TRUE(currency.code().empty());
  }
  for (std::string code : {std::string("\0NY", 3), std::string("C\0Y", 3),
                           std::string("CN\0", 3)}) {
    const Currency currency(code);
    EXPECT_FALSE(currency.valid());
    EXPECT_TRUE(currency.code().empty());
  }
}

TEST(CurrencyTest, OwnsTheCodeIndependentlyOfItsInputAndOtherCopies) {
  std::string input = "CNY";
  const Currency original(input);
  auto copy = original;
  input.assign("USD");
  EXPECT_EQ(original.code(), "CNY");
  EXPECT_EQ(copy, original);
  copy = Currency(input);
  EXPECT_EQ(copy.code(), "USD");
  EXPECT_NE(copy, original);
  EXPECT_EQ(original.code(), "CNY");
}

}  // namespace
}  // namespace yquant::model
