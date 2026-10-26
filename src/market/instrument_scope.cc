#include "yquant/market/instrument_scope.h"

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "yquant/base/config_error.h"

namespace yquant::market {

size_t InstrumentScope::Hash(model::InstrumentId id) noexcept {
  auto value = id.value;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return (value ^ (value >> 31)) & (kLookupCapacity - 1);
}

base::Status InstrumentScope::Configure(
    const MarketDataConfig& config) noexcept {
  if (source_id_.value != 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  base::ConfigError error;
  const auto status = ValidateMarketDataScope(config, error);
  if (!status.ok()) {
    return status;
  }

  source_id_ = config.source_id;
  instrument_count_ = config.instrument_count;
  for (size_t index = 0; index < instrument_count_; ++index) {
    const auto& definition = config.instruments[index].instrument;
    definitions_[index] = definition;
    auto position = Hash(definition.id);
    while (lookup_[position].index_plus_one != 0) {
      position = (position + 1) & (kLookupCapacity - 1);
    }
    lookup_[position] = {definition.id, static_cast<uint8_t>(index + 1)};
  }
  return {};
}

void InstrumentScope::Reset() noexcept {
  source_id_ = {};
  instrument_count_ = 0;
  definitions_.fill({});
  lookup_.fill({});
}

std::optional<size_t> InstrumentScope::FindIndex(
    model::InstrumentId id) const noexcept {
  if (id.value == 0) {
    return {};
  }
  auto position = Hash(id);
  for (size_t scanned = 0; scanned < kLookupCapacity; ++scanned) {
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

bool InstrumentScope::TryGetInstrument(
    model::InstrumentId id, model::InstrumentDefinition& out) const noexcept {
  const auto index = FindIndex(id);
  if (!index) {
    return false;
  }
  out = definitions_[*index];
  return true;
}

}  // namespace yquant::market
