#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>

#include "gtest/gtest.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"

namespace yquant::model {
namespace {

using base::ErrorCode;

static_assert(std::is_trivially_copyable_v<SourceId>);
static_assert(std::is_trivially_copyable_v<InstrumentId>);
static_assert(std::is_trivially_copyable_v<InstrumentDefinition>);
static_assert(std::is_trivially_copyable_v<QuoteTick>);
static_assert(!std::is_convertible_v<SourceId, InstrumentId>);

TEST(MarketModelTest, InstrumentIdentityUsesStableVenueCodes) {
  InstrumentId sse;
  InstrumentId szse;
  InstrumentId bse;
  ASSERT_EQ(MakeInstrumentId("SSE", "600000", sse), ErrorCode::kOk);
  ASSERT_EQ(MakeInstrumentId("SZSE", "600000", szse), ErrorCode::kOk);
  ASSERT_EQ(MakeInstrumentId("BSE", "600000", bse), ErrorCode::kOk);
  EXPECT_EQ(sse.value, (std::uint64_t{1} << 32) | 600000);
  EXPECT_EQ(szse.value, (std::uint64_t{2} << 32) | 600000);
  EXPECT_EQ(bse.value, (std::uint64_t{3} << 32) | 600000);
  EXPECT_NE(sse, szse);
  EXPECT_NE(sse, bse);

  // Resolving another venue first cannot change an existing instrument's ID.
  InstrumentId same_sse;
  ASSERT_EQ(MakeInstrumentId("SSE", "600000", same_sse), ErrorCode::kOk);
  EXPECT_EQ(same_sse, sse);
}

TEST(MarketModelTest, SixDigitIdentityRetainsLeadingZerosInDefinition) {
  InstrumentDefinition definition;
  ASSERT_EQ(MakeInstrumentId("SZSE", "000001", definition.id), ErrorCode::kOk);
  definition.venue = Venue::kSzse;
  std::memcpy(definition.symbol, "000001", sizeof(definition.symbol));
  definition.price_decimals = 2;

  EXPECT_EQ(definition.id.value, (std::uint64_t{2} << 32) | 1);
  EXPECT_EQ(definition.symbol_view(), "000001");
  const auto copied = definition;
  std::memset(definition.symbol, '9', sizeof(definition.symbol));
  EXPECT_EQ(copied.symbol_view(), "000001");
}

TEST(MarketModelTest, InvalidInstrumentInputPreservesOutput) {
  const struct {
    std::string_view venue;
    std::string_view symbol;
  } cases[] = {
      {"", "600000"},
      {"sse", "600000"},
      {"UNKNOWN", "600000"},
      {"SSE", ""},
      {"SSE", "60000"},
      {"SSE", "6000000"},
      {"SSE", "60000A"},
      {"SSE", "+00001"},
      {"SSE", " 00001"},
      {"SSE", "６０００００"},
      {"SSE",
       {"600\0"
        "00",
        6}},
  };
  for (const auto& input : cases) {
    SCOPED_TRACE(input.venue);
    SCOPED_TRACE(input.symbol);
    InstrumentId id{123456};
    EXPECT_EQ(MakeInstrumentId(input.venue, input.symbol, id),
              ErrorCode::kInvalidArgument);
    EXPECT_EQ(id, InstrumentId{123456});
  }
}

TEST(MarketModelTest, PresenceUsesFlagsRatherThanZeroPrices) {
  QuoteTick quote;
  EXPECT_FALSE(HasQuoteFlag(quote.flags, QuoteFlag::kBidValid));
  quote.bid_price = 0;
  quote.bid_qty = 100;
  SetQuoteFlag(quote.flags, QuoteFlag::kBidValid);
  EXPECT_TRUE(HasQuoteFlag(quote.flags, QuoteFlag::kBidValid));
  EXPECT_FALSE(HasQuoteFlag(quote.flags, QuoteFlag::kAskValid));
  EXPECT_EQ(quote.flags & ~kKnownQuoteFlags, 0);
}

TEST(MarketModelTest, SettingQuoteFlagPreservesOtherBitsAndIsIdempotent) {
  std::uint8_t flags = 0x80 | static_cast<std::uint8_t>(QuoteFlag::kAskValid);
  const auto preserved_flags = flags;

  SetQuoteFlag(flags, QuoteFlag::kBidValid);
  EXPECT_TRUE(HasQuoteFlag(flags, QuoteFlag::kBidValid));
  EXPECT_EQ(flags & ~static_cast<std::uint8_t>(QuoteFlag::kBidValid),
            preserved_flags);
  const auto enabled_flags = flags;
  SetQuoteFlag(flags, QuoteFlag::kBidValid);
  EXPECT_EQ(flags, enabled_flags);

  SetQuoteFlag(flags, QuoteFlag::kBidValid, false);
  EXPECT_EQ(flags, preserved_flags);
  SetQuoteFlag(flags, QuoteFlag::kBidValid, false);
  EXPECT_EQ(flags, preserved_flags);
}

TEST(MarketModelTest, SnapshotEqualityIgnoresPaddingAndChecksAllMembers) {
  QuoteTick first;
  QuoteTick second;
  // Differing padding must not make equivalent cache/callback values unequal.
  std::memset(&first, 0xa5, sizeof(first));
  std::memset(&second, 0x5a, sizeof(second));
  auto initialize = [](QuoteTick& quote) {
    quote.source_id = SourceId{1};
    quote.instrument_id = InstrumentId{(std::uint64_t{1} << 32) | 600000};
    quote.sequence = 7;
    quote.event_time_ns = 100;
    quote.receive_time_ns = 200;
    quote.bid_price = 1000;
    quote.bid_qty = 100;
    quote.ask_price = 1001;
    quote.ask_qty = 200;
    quote.flags = kKnownQuoteFlags;
  };
  initialize(first);
  initialize(second);
  EXPECT_EQ(first, second);

  second.source_id = SourceId{2};
  EXPECT_NE(first, second);
  initialize(second);
  ++second.sequence;
  EXPECT_NE(first, second);
  initialize(second);
  ++second.ask_qty;
  EXPECT_NE(first, second);
  initialize(second);
  second.flags = 0;
  EXPECT_NE(first, second);
}

}  // namespace
}  // namespace yquant::model
