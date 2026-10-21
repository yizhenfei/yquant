#include <cstdint>
#include <cstring>
#include <set>
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
  EXPECT_TRUE(IsValidInstrumentId(sse));

  // Resolving another venue first cannot change an existing instrument's ID.
  InstrumentId same_sse;
  ASSERT_EQ(MakeInstrumentId("SSE", "600000", same_sse), ErrorCode::kOk);
  EXPECT_EQ(same_sse, sse);
}

TEST(MarketModelTest, FuturesIdentityHasStableVenueAndCanonicalProductMonth) {
  const struct {
    std::string_view venue;
    std::string_view symbol;
    Venue code;
  } cases[] = {
      {"SHFE", "rb2501", Venue::kShfe}, {"DCE", "m2505", Venue::kDce},
      {"CZCE", "SR501", Venue::kCzce},  {"CFFEX", "IF2506", Venue::kCffex},
      {"INE", "sc2512", Venue::kIne},   {"GFEX", "si2601", Venue::kGfex}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.venue);
    InstrumentId id;
    ASSERT_EQ(MakeInstrumentId(item.venue, item.symbol, id), ErrorCode::kOk);
    EXPECT_EQ(id.value >> 32, static_cast<std::uint64_t>(item.code));
    EXPECT_NE(id.value & 0x80000000U, 0U);
    EXPECT_TRUE(IsValidInstrumentId(id));
    EXPECT_EQ(VenueName(item.code), item.venue);
    EXPECT_TRUE(IsFuturesVenue(item.code));
  }
  InstrumentId rb;
  ASSERT_EQ(MakeInstrumentId("SHFE", "rb2501", rb), ErrorCode::kOk);
  EXPECT_EQ(rb.value, (std::uint64_t{4} << 32) | 0x80000000U |
                          ((18U * 27 + 2) << 14) | 3501U);
  EXPECT_FALSE(IsFuturesVenue(Venue::kSse));
  EXPECT_TRUE(VenueName(static_cast<Venue>(255)).empty());
}

TEST(MarketModelTest, ProductLengthAndLeadingYearZerosCannotAlias) {
  std::set<std::uint64_t> ids;
  // Base-27 digits are all nonzero, so varying product length is unambiguous.
  for (std::string_view symbol : {"a0001", "aa0001", "aaa0001", "z9912",
                                  "zz9912", "zzz9912", "a0002", "a0101"}) {
    InstrumentId id;
    ASSERT_EQ(MakeInstrumentId("DCE", symbol, id), ErrorCode::kOk);
    EXPECT_TRUE(IsValidInstrumentId(id));
    EXPECT_TRUE(ids.insert(id.value).second);
  }
  for (std::string_view symbol : {"A001", "AA001", "AAA001", "ZZZ912"}) {
    InstrumentId id;
    ASSERT_EQ(MakeInstrumentId("CZCE", symbol, id), ErrorCode::kOk);
    EXPECT_TRUE(IsValidInstrumentId(id));
    EXPECT_TRUE(ids.insert(id.value).second);
  }
}

TEST(MarketModelTest, InvalidFuturesInputPreservesOutput) {
  const struct {
    std::string_view venue;
    std::string_view symbol;
  } cases[] = {{"SHFE", "RB2501"},
               {"DCE", "i501"},
               {"INE", "sc2500"},
               {"GFEX", "si2513"},
               {"CZCE", "sr501"},
               {"CZCE", "SR2501"},
               {"CFFEX", "if2501"},
               {"CFFEX", "IF501"},
               {"SHFE", "2501"},
               {"SHFE", "abcd2501"},
               {"SHFE", "rB2501"},
               {"SHFE", "rb25a1"},
               {"SHFE", "rb+501"},
               {"SHFE", "rb2501 "},
               {"shfe", "rb2501"},
               {"SHFE",
                {"rb2\0"
                 "01",
                 6}}};
  for (const auto& item : cases) {
    SCOPED_TRACE(item.venue);
    SCOPED_TRACE(item.symbol);
    InstrumentId id{99};
    EXPECT_EQ(MakeInstrumentId(item.venue, item.symbol, id),
              ErrorCode::kInvalidArgument);
    EXPECT_EQ(id.value, 99U);
  }
}

TEST(MarketModelTest, RejectsNoncanonicalEncodedIdentities) {
  constexpr auto kMarker = std::uint64_t{0x80000000U};
  constexpr auto kDce = std::uint64_t{5} << 32;
  constexpr auto kCzce = std::uint64_t{6} << 32;
  for (const auto value :
       {std::uint64_t{0}, std::uint64_t{10} << 32,
        (std::uint64_t{257} << 32) | 1, (std::uint64_t{1} << 32) | kMarker,
        kDce | (1U << 14) | 3501U, kDce | kMarker | 3501U,
        kDce | kMarker | (27U << 14) | 3501U,
        kDce | kMarker | (19683U << 14) | 3501U,
        kDce | kMarker | (1U << 14) | 501U, kDce | kMarker | (1U << 14) | 1000U,
        kDce | kMarker | (1U << 14) | 3513U,
        kCzce | kMarker | (1U << 14) | 1501U}) {
    SCOPED_TRACE(value);
    EXPECT_FALSE(IsValidInstrumentId({value}));
  }
}

TEST(MarketModelTest, InstrumentOwnsBoundedSymbolsAndEquityDefaults) {
  InstrumentDefinition instrument;
  EXPECT_EQ(instrument.type, InstrumentType::kEquity);
  EXPECT_EQ(instrument.price_tick, 1);
  EXPECT_EQ(instrument.quantity_step, 1);
  EXPECT_EQ(instrument.contract_multiplier, 1);
  EXPECT_EQ(instrument.margin_rate_ppm, 0);
  EXPECT_EQ(instrument.open_fee_per_lot, 0);
  EXPECT_EQ(instrument.close_today_fee_per_lot, 0);
  EXPECT_EQ(instrument.close_yesterday_fee_per_lot, 0);
  std::memcpy(instrument.symbol, "rb2501", 6);
  const auto copy = instrument;
  std::memset(instrument.symbol, 'a', sizeof(instrument.symbol));
  EXPECT_EQ(instrument.symbol_view().size(), sizeof(instrument.symbol));
  EXPECT_EQ(copy.symbol_view(), "rb2501");
}

TEST(MarketModelTest, SixDigitIdentityRetainsLeadingZerosInDefinition) {
  InstrumentDefinition definition;
  ASSERT_EQ(MakeInstrumentId("SZSE", "000001", definition.id), ErrorCode::kOk);
  definition.venue = Venue::kSzse;
  std::memcpy(definition.symbol, "000001", 6);
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
  quote.flags = static_cast<std::uint8_t>(QuoteFlag::kBidValid);
  EXPECT_TRUE(HasQuoteFlag(quote.flags, QuoteFlag::kBidValid));
  EXPECT_FALSE(HasQuoteFlag(quote.flags, QuoteFlag::kAskValid));
  EXPECT_EQ(quote.flags & ~kKnownQuoteFlags, 0);
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
