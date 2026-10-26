#include "strategyd/market_data.h"

#include <stdint.h>

#include <string_view>

namespace yquant::strategyd {
namespace {

// Checks a book side's presence encoding. A present side requires positive
// quantity but may have a zero or negative price; absent fields must be zero.
bool ValidSide(uint8_t flags, model::QuoteFlag side, int64_t price,
               int64_t quantity) noexcept {
  return model::HasQuoteFlag(flags, side) ? quantity > 0
                                          : price == 0 && quantity == 0;
}

}  // namespace

base::Status MarketDataImpl::Configure(
    const market::MarketDataConfig& config) noexcept {
  if (phase_ != strategy::CallbackPhase::kNone) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  return instruments_.Configure(config);
}

void MarketDataImpl::Reset() noexcept {
  instruments_.Reset();
  snapshots_.fill({});
  subscriptions_.fill({});
  subscription_count_ = 0;
  phase_ = strategy::CallbackPhase::kNone;
}

base::ErrorCode MarketDataImpl::FindInstrument(
    std::string_view venue, std::string_view symbol,
    model::InstrumentDefinition& out) const noexcept {
  model::InstrumentId id;
  if (model::MakeInstrumentId(venue, symbol, id) != base::ErrorCode::kOk) {
    return base::ErrorCode::kInvalidArgument;
  }
  const auto index = instruments_.FindIndex(id);
  if (!index) {
    return base::ErrorCode::kInvalidArgument;
  }
  out = instruments_[*index];
  return base::ErrorCode::kOk;
}

base::ErrorCode MarketDataImpl::SubscribeQuotes(
    model::InstrumentId id) noexcept {
  if (phase_ != strategy::CallbackPhase::kStart) {
    return base::ErrorCode::kInvalidState;
  }
  if (instruments_.source_id().value == 0) {
    return base::ErrorCode::kUnsupported;
  }
  const auto index = instruments_.FindIndex(id);
  if (!index) {
    return base::ErrorCode::kInvalidArgument;
  }
  auto& slot = snapshots_[*index];
  if (!slot.subscribed) {
    subscriptions_[subscription_count_++] = id;
    slot.subscribed = true;
  }
  return base::ErrorCode::kOk;
}

bool MarketDataImpl::TryGetLatestQuote(model::InstrumentId id,
                                       model::QuoteTick& out) const noexcept {
  const auto index = instruments_.FindIndex(id);
  if (instruments_.source_id().value == 0 || !index) {
    return false;
  }
  const auto& slot = snapshots_[*index];
  if (!slot.subscribed || !slot.has_quote) {
    return false;
  }
  out = slot.latest;
  return true;
}

base::ErrorCode MarketDataImpl::ApplyQuote(const model::QuoteTick& tick,
                                           bool& accepted) noexcept {
  const auto index = instruments_.FindIndex(tick.instrument_id);
  if (tick.source_id != instruments_.source_id() || !index ||
      !snapshots_[*index].subscribed || tick.sequence == 0 ||
      (tick.flags & ~model::kKnownQuoteFlags) != 0 ||
      tick.receive_time_ns > clock_.Now() ||
      (!model::HasQuoteFlag(tick.flags, model::QuoteFlag::kEventTimeValid) &&
       tick.event_time_ns != 0) ||
      !ValidSide(tick.flags, model::QuoteFlag::kBidValid, tick.bid_price,
                 tick.bid_qty) ||
      !ValidSide(tick.flags, model::QuoteFlag::kAskValid, tick.ask_price,
                 tick.ask_qty)) {
    return base::ErrorCode::kCorrupt;
  }
  const auto& definition = instruments_[*index];
  if (tick.bid_price % definition.price_tick != 0 ||
      tick.ask_price % definition.price_tick != 0 ||
      tick.bid_qty % definition.quantity_step != 0 ||
      tick.ask_qty % definition.quantity_step != 0) {
    return base::ErrorCode::kCorrupt;
  }
  auto& slot = snapshots_[*index];
  // Validate even old quotes before ignoring them so stale sequence numbers
  // cannot hide malformed input from the configured source.
  if (slot.has_quote && tick.sequence <= slot.latest.sequence) {
    accepted = false;
    return base::ErrorCode::kOk;
  }
  slot.latest = tick;
  slot.has_quote = true;
  accepted = true;
  return base::ErrorCode::kOk;
}

}  // namespace yquant::strategyd
