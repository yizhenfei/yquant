#include "strategyd/market_data.h"

namespace yquant::strategyd {
namespace {

// Presence bits own the interpretation of zero fields, not the price itself.
bool ValidSide(std::uint8_t flags, model::QuoteFlag side, std::int64_t price,
               std::int64_t quantity) noexcept {
  return model::HasQuoteFlag(flags, side) ? quantity > 0
                                          : price == 0 && quantity == 0;
}

}  // namespace

std::size_t MarketDataImpl::Hash(model::InstrumentId id) noexcept {
  auto value = id.value;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return (value ^ (value >> 31)) & (kLookupCapacity - 1);
}

std::optional<std::size_t> MarketDataImpl::FindIndex(
    model::InstrumentId id) const noexcept {
  if (id.value == 0) {
    return {};
  }
  auto position = Hash(id);
  for (std::size_t scanned = 0; scanned < kLookupCapacity; ++scanned) {
    const auto& slot = lookup_[position];
    if (slot.index_plus_one == 0) {
      return {};
    }
    if (slot.id == id) {
      return slot.index_plus_one - 1;
    }
    position = (position + 1) & (kLookupCapacity - 1);
  }
  return {};
}

base::Status MarketDataImpl::Configure(
    const MarketDataConfig& config) noexcept {
  if (phase_ != CallbackPhase::kNone || instrument_count_ != 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  strategy::ConfigError error;
  const auto result = ValidateMarketDataConfig(config, error);
  if (!result.ok()) {
    return result;
  }
  source_id_ = config.source_id;
  instrument_count_ = config.instrument_count;
  for (std::size_t index = 0; index < instrument_count_; ++index) {
    const auto& definition = config.instruments[index].instrument;
    instruments_[index].definition = definition;
    auto position = Hash(definition.id);
    while (lookup_[position].index_plus_one != 0) {
      position = (position + 1) & (kLookupCapacity - 1);
    }
    lookup_[position] = {definition.id, static_cast<std::uint8_t>(index + 1)};
  }
  return {};
}

void MarketDataImpl::Reset() noexcept {
  source_id_ = {};
  instrument_count_ = 0;
  instruments_.fill({});
  lookup_.fill({});
  subscriptions_.fill({});
  subscription_count_ = 0;
  phase_ = CallbackPhase::kNone;
}

base::ErrorCode MarketDataImpl::FindInstrument(
    std::string_view venue, std::string_view symbol,
    model::InstrumentDefinition& out) const noexcept {
  model::InstrumentId id;
  if (model::MakeInstrumentId(venue, symbol, id) != base::ErrorCode::kOk) {
    return base::ErrorCode::kInvalidArgument;
  }
  const auto index = FindIndex(id);
  if (!index) {
    return base::ErrorCode::kInvalidArgument;
  }
  out = instruments_[*index].definition;
  return base::ErrorCode::kOk;
}

base::ErrorCode MarketDataImpl::SubscribeQuotes(
    model::SourceId source_id, model::InstrumentId id) noexcept {
  if (phase_ != CallbackPhase::kStart) {
    return base::ErrorCode::kInvalidState;
  }
  if (source_id_.value == 0) {
    return base::ErrorCode::kUnsupported;
  }
  const auto index = FindIndex(id);
  if (source_id != source_id_ || !index) {
    return base::ErrorCode::kInvalidArgument;
  }
  auto& slot = instruments_[*index];
  if (!slot.subscribed) {
    subscriptions_[subscription_count_++] = id;
    slot.subscribed = true;
  }
  return base::ErrorCode::kOk;
}

bool MarketDataImpl::TryGetLatestQuote(model::SourceId source_id,
                                       model::InstrumentId id,
                                       model::QuoteTick& out) const noexcept {
  const auto index = FindIndex(id);
  if (source_id != source_id_ || !index) {
    return false;
  }
  const auto& slot = instruments_[*index];
  if (!slot.subscribed || !slot.has_quote) {
    return false;
  }
  out = slot.latest;
  return true;
}

base::ErrorCode MarketDataImpl::ApplyQuote(const model::QuoteTick& tick,
                                           bool& accepted) noexcept {
  const auto index = FindIndex(tick.instrument_id);
  if (tick.source_id != source_id_ || !index ||
      !instruments_[*index].subscribed || tick.sequence == 0 ||
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
  auto& slot = instruments_[*index];
  const auto& definition = slot.definition;
  if (tick.bid_price % definition.price_tick != 0 ||
      tick.ask_price % definition.price_tick != 0 ||
      tick.bid_qty % definition.quantity_step != 0 ||
      tick.ask_qty % definition.quantity_step != 0) {
    return base::ErrorCode::kCorrupt;
  }
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
