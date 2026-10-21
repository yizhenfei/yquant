#include "yquant/md/quote_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "gtest/gtest.h"

namespace yquant::md {
namespace {

model::QuoteTick Quote() {
  model::QuoteTick quote;
  quote.source_id.value = 0x12345678;
  quote.instrument_id.value = (std::uint64_t{2} << 32) | 600001;
  quote.sequence = 0xfedcba9876543210ULL;
  quote.event_time_ns = 1'701'234'567'890'123'456;
  quote.receive_time_ns = 123456;
  quote.bid_price = -987654321;
  quote.bid_qty = 100;
  quote.ask_price = 0;
  quote.ask_qty = 200;
  quote.flags = model::kKnownQuoteFlags;
  return quote;
}

TEST(QuoteCodecTest, RoundTripRetainsSourceAndSignedValues) {
  const auto original = Quote();
  QuoteFrame frame;
  ASSERT_TRUE(EncodeQuote(original, frame).ok());
  model::QuoteTick decoded;
  ASSERT_TRUE(DecodeQuote(frame, decoded).ok());
  auto expected = original;
  expected.receive_time_ns = 0;
  EXPECT_EQ(decoded, expected);
  EXPECT_EQ(original.receive_time_ns, 123456);
  // Source IDs are encoded in little endian, independent of native padding.
  EXPECT_EQ(frame[8], std::byte{0x78});
  EXPECT_EQ(frame[9], std::byte{0x56});
  EXPECT_EQ(frame[10], std::byte{0x34});
  EXPECT_EQ(frame[11], std::byte{0x12});
}

TEST(QuoteCodecTest, FuturesIdentitiesUseTheExistingWireLayout) {
  for (const auto& [venue, symbol] :
       {std::pair{"SHFE", "rb2501"}, std::pair{"DCE", "m2505"},
        std::pair{"CZCE", "SR501"}, std::pair{"CFFEX", "IF2506"},
        std::pair{"INE", "sc2512"}, std::pair{"GFEX", "si2601"}}) {
    SCOPED_TRACE(venue);
    auto quote = Quote();
    ASSERT_EQ(model::MakeInstrumentId(venue, symbol, quote.instrument_id),
              base::ErrorCode::kOk);
    QuoteFrame frame;
    ASSERT_TRUE(EncodeQuote(quote, frame).ok());
    EXPECT_EQ(frame.size(), 80U);
    model::QuoteTick decoded;
    ASSERT_TRUE(DecodeQuote(frame, decoded).ok());
    quote.receive_time_ns = 0;
    EXPECT_EQ(decoded, quote);
    // A marker alone has no product/month and must not pass the decoder.
    for (std::size_t byte = 16; byte != 20; ++byte) frame[byte] = std::byte{0};
    frame[19] = std::byte{0x80};
    auto sentinel = Quote();
    EXPECT_EQ(DecodeQuote(frame, sentinel).code, base::ErrorCode::kCorrupt);
    EXPECT_EQ(sentinel, Quote());
  }
}

TEST(QuoteCodecTest, ReceiveClockIsNeitherValidatedNorTransmitted) {
  auto quote = Quote();
  QuoteFrame first;
  ASSERT_TRUE(EncodeQuote(quote, first).ok());
  quote.receive_time_ns = std::numeric_limits<std::uint64_t>::max();
  EXPECT_TRUE(ValidateQuote(quote).ok());
  QuoteFrame second;
  ASSERT_TRUE(EncodeQuote(quote, second).ok());
  EXPECT_EQ(first, second);
  model::QuoteTick decoded = quote;
  ASSERT_TRUE(DecodeQuote(first, decoded).ok());
  EXPECT_EQ(decoded.receive_time_ns, 0);
}

TEST(QuoteCodecTest, MissingSidesAndEventTimeUseCanonicalZeros) {
  auto quote = Quote();
  quote.flags = 0;
  quote.event_time_ns = 0;
  quote.bid_price = quote.bid_qty = quote.ask_price = quote.ask_qty = 0;
  EXPECT_TRUE(ValidateQuote(quote).ok());
  QuoteFrame frame;
  ASSERT_TRUE(EncodeQuote(quote, frame).ok());
  model::QuoteTick decoded;
  ASSERT_TRUE(DecodeQuote(frame, decoded).ok());
  quote.receive_time_ns = 0;
  EXPECT_EQ(decoded, quote);
  quote.bid_qty = 1;
  EXPECT_FALSE(ValidateQuote(quote).ok());
  quote.bid_qty = 0;
  quote.event_time_ns = 1;
  EXPECT_FALSE(ValidateQuote(quote).ok());
}

TEST(QuoteCodecTest, InvalidQuotesPreserveEncodingOutput) {
  const auto reject = [](model::QuoteTick quote) {
    QuoteFrame output;
    output.fill(std::byte{0x5a});
    const auto before = output;
    EXPECT_EQ(ValidateQuote(quote).code, base::ErrorCode::kCorrupt);
    EXPECT_EQ(EncodeQuote(quote, output).code, base::ErrorCode::kCorrupt);
    EXPECT_EQ(output, before);
  };
  auto quote = Quote();
  quote.source_id.value = 0;
  reject(quote);
  quote = Quote();
  quote.sequence = 0;
  reject(quote);
  for (const auto id : {std::uint64_t{0}, std::uint64_t{4} << 32,
                        (std::uint64_t{1} << 32) | 1000000}) {
    quote = Quote();
    quote.instrument_id.value = id;
    reject(quote);
  }
  quote = Quote();
  quote.flags |= 0x80;
  reject(quote);
  quote = Quote();
  quote.bid_qty = 0;
  reject(quote);
  quote = Quote();
  quote.ask_qty = -1;
  reject(quote);
  quote = Quote();
  quote.flags &= ~static_cast<std::uint8_t>(model::QuoteFlag::kBidValid);
  reject(quote);
  quote = Quote();
  quote.flags &= ~static_cast<std::uint8_t>(model::QuoteFlag::kAskValid);
  reject(quote);
  quote = Quote();
  quote.flags &= ~static_cast<std::uint8_t>(model::QuoteFlag::kEventTimeValid);
  reject(quote);
}

TEST(QuoteCodecTest, InvalidFramesPreserveDecodingOutput) {
  QuoteFrame valid;
  ASSERT_TRUE(EncodeQuote(Quote(), valid).ok());
  const auto sentinel = Quote();
  const auto reject = [&](std::span<const std::byte> bytes) {
    auto output = sentinel;
    EXPECT_EQ(DecodeQuote(bytes, output).code, base::ErrorCode::kCorrupt);
    EXPECT_EQ(output, sentinel);
  };
  reject({});
  reject(std::span(valid).first(kQuoteFrameSize - 1));
  std::array<std::byte, kQuoteFrameSize + 1> oversized{};
  std::copy(valid.begin(), valid.end(), oversized.begin());
  reject(oversized);
  // Magic, schema version, declared size, flags and reserved fields.
  for (std::size_t offset : {0, 4, 6, 13, 14, 15, 72, 79}) {
    auto broken = valid;
    broken[offset] ^= std::byte{0xff};
    reject(broken);
  }
  auto unknown_flags = valid;
  unknown_flags[12] |= std::byte{0x80};
  reject(unknown_flags);
  auto source_zero = valid;
  std::fill(source_zero.begin() + 8, source_zero.begin() + 12, std::byte{0});
  reject(source_zero);
  auto invalid_quantity = valid;
  std::fill(invalid_quantity.begin() + 48, invalid_quantity.begin() + 56,
            std::byte{0});
  reject(invalid_quantity);
}

TEST(QuoteCodecTest, EventEnvelopeMatchesCompleteFrameIdentity) {
  const auto quote = Quote();
  QuoteFrame frame;
  ASSERT_TRUE(EncodeQuote(quote, frame).ok());
  const auto event = QuoteEvent(quote, frame);
  EXPECT_EQ(event.payload.data(), frame.data());
  EXPECT_EQ(event.payload.size(), kQuoteFrameSize);
  EXPECT_EQ(event.header.source_id, quote.source_id.value);
  EXPECT_EQ(event.header.instrument_id, quote.instrument_id.value);
  EXPECT_EQ(event.header.source_sequence, quote.sequence);
  EXPECT_EQ(event.header.source_timestamp_ns, quote.event_time_ns);
  EXPECT_TRUE(event.header.has_source_sequence);
  EXPECT_EQ(event.header.type, kQuoteEventType);
  EXPECT_EQ(event.header.scope, EventScope::kBusiness);
  auto expected = quote;
  expected.receive_time_ns = 0;
  model::QuoteTick output;
  ASSERT_TRUE(DecodeQuoteEvent(event, output).ok());
  EXPECT_EQ(output, expected);

  const auto reject = [&](EventView invalid) {
    auto sentinel = Quote();
    EXPECT_EQ(DecodeQuoteEvent(invalid, sentinel).code,
              base::ErrorCode::kCorrupt);
    EXPECT_EQ(sentinel, quote);
  };
  auto invalid = event;
  ++invalid.header.source_id;
  reject(invalid);
  invalid = event;
  ++invalid.header.instrument_id;
  reject(invalid);
  invalid = event;
  ++invalid.header.source_sequence;
  reject(invalid);
  invalid = event;
  ++invalid.header.source_timestamp_ns;
  reject(invalid);
  invalid = event;
  invalid.header.has_source_sequence = false;
  reject(invalid);
  invalid = event;
  ++invalid.header.type;
  reject(invalid);
  invalid = event;
  invalid.header.scope = EventScope::kStreamStatus;
  reject(invalid);
  invalid = event;
  invalid.payload = invalid.payload.first(kQuoteFrameSize - 1);
  reject(invalid);
}

}  // namespace
}  // namespace yquant::md
