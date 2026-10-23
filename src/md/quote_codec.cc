#include "yquant/md/quote_codec.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>

namespace yquant::md {
namespace {

constexpr uint32_t kQuoteMagic = 0x31515159;
constexpr uint16_t kQuoteVersion = 1;

// The byte frame is independent of native struct padding and endianness.
template <typename T>
void Put(QuoteFrame& frame, size_t offset, T value) noexcept {
  for (size_t index = 0; index < sizeof(T); ++index) {
    frame[offset + index] = static_cast<std::byte>(value & 0xffU);
    value >>= 8;
  }
}

template <typename T>
T Get(std::span<const std::byte> frame, size_t offset) noexcept {
  T value = 0;
  for (size_t index = 0; index < sizeof(T); ++index) {
    value |=
        static_cast<T>(std::to_integer<unsigned int>(frame[offset + index]))
        << (8 * index);
  }
  return value;
}

// A missing side is canonically zero; present sides may have signed prices.
bool ValidSide(uint8_t flags, model::QuoteFlag side, int64_t price,
               int64_t quantity) noexcept {
  return model::HasQuoteFlag(flags, side) ? quantity > 0
                                          : price == 0 && quantity == 0;
}

}  // namespace

base::Status ValidateQuote(const model::QuoteTick& quote) noexcept {
  if (quote.source_id.value == 0 ||
      !model::IsValidInstrumentId(quote.instrument_id) || quote.sequence == 0 ||
      (quote.flags & ~model::kKnownQuoteFlags) != 0 ||
      (!model::HasQuoteFlag(quote.flags, model::QuoteFlag::kEventTimeValid) &&
       quote.event_time_ns != 0) ||
      !ValidSide(quote.flags, model::QuoteFlag::kBidValid, quote.bid_price,
                 quote.bid_qty) ||
      !ValidSide(quote.flags, model::QuoteFlag::kAskValid, quote.ask_price,
                 quote.ask_qty)) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  return {};
}

base::Status EncodeQuote(const model::QuoteTick& quote,
                         QuoteFrame& out) noexcept {
  const auto result = ValidateQuote(quote);
  if (!result.ok()) {
    return result;
  }
  QuoteFrame frame{};
  Put(frame, 0, kQuoteMagic);
  Put(frame, 4, kQuoteVersion);
  Put(frame, 6, static_cast<uint16_t>(kQuoteFrameSize));
  Put(frame, 8, quote.source_id.value);
  frame[12] = static_cast<std::byte>(quote.flags);
  Put(frame, 16, quote.instrument_id.value);
  Put(frame, 24, quote.sequence);
  Put(frame, 32, quote.event_time_ns);
  Put(frame, 40, std::bit_cast<uint64_t>(quote.bid_price));
  Put(frame, 48, std::bit_cast<uint64_t>(quote.bid_qty));
  Put(frame, 56, std::bit_cast<uint64_t>(quote.ask_price));
  Put(frame, 64, std::bit_cast<uint64_t>(quote.ask_qty));
  out = frame;
  return {};
}

base::Status DecodeQuote(std::span<const std::byte> frame,
                         model::QuoteTick& out) noexcept {
  if (frame.size() != kQuoteFrameSize ||
      Get<uint32_t>(frame, 0) != kQuoteMagic ||
      Get<uint16_t>(frame, 4) != kQuoteVersion ||
      Get<uint16_t>(frame, 6) != kQuoteFrameSize ||
      std::any_of(frame.begin() + 13, frame.begin() + 16,
                  [](std::byte value) { return value != std::byte{0}; }) ||
      std::any_of(frame.begin() + 72, frame.end(),
                  [](std::byte value) { return value != std::byte{0}; })) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  model::QuoteTick quote;
  quote.source_id.value = Get<uint32_t>(frame, 8);
  quote.flags = std::to_integer<uint8_t>(frame[12]);
  quote.instrument_id.value = Get<uint64_t>(frame, 16);
  quote.sequence = Get<uint64_t>(frame, 24);
  quote.event_time_ns = Get<uint64_t>(frame, 32);
  quote.bid_price = std::bit_cast<int64_t>(Get<uint64_t>(frame, 40));
  quote.bid_qty = std::bit_cast<int64_t>(Get<uint64_t>(frame, 48));
  quote.ask_price = std::bit_cast<int64_t>(Get<uint64_t>(frame, 56));
  quote.ask_qty = std::bit_cast<int64_t>(Get<uint64_t>(frame, 64));
  const auto result = ValidateQuote(quote);
  if (!result.ok()) {
    return result;
  }
  out = quote;
  return {};
}

EventView QuoteEvent(const model::QuoteTick& quote,
                     const QuoteFrame& frame) noexcept {
  EventHeader header;
  header.source_id = quote.source_id.value;
  header.instrument_id = quote.instrument_id.value;
  header.source_sequence = quote.sequence;
  header.has_source_sequence = true;
  header.source_timestamp_ns = quote.event_time_ns;
  header.type = kQuoteEventType;
  header.scope = EventScope::kBusiness;
  return {header, frame};
}

base::Status DecodeQuoteEvent(const EventView& event,
                              model::QuoteTick& out) noexcept {
  model::QuoteTick quote;
  auto result = DecodeQuote(event.payload, quote);
  if (!result.ok()) {
    return result;
  }
  if (event.header.scope != EventScope::kBusiness ||
      event.header.type != kQuoteEventType ||
      event.header.source_id != quote.source_id.value ||
      event.header.instrument_id != quote.instrument_id.value ||
      !event.header.has_source_sequence ||
      event.header.source_sequence != quote.sequence ||
      event.header.source_timestamp_ns != quote.event_time_ns) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  out = quote;
  return {};
}

}  // namespace yquant::md
