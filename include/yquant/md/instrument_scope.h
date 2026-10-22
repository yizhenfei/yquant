#ifndef YQUANT_MD_INSTRUMENT_SCOPE_H_
#define YQUANT_MD_INSTRUMENT_SCOPE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "yquant/base/status.h"
#include "yquant/md/market_data_config.h"
#include "yquant/model/instrument.h"

namespace yquant::md {

// Owns one source's immutable instrument definitions and a fixed lookup table.
// Definitions retain configuration order. Configure, Reset, and queries must
// be serialized by the owner; no operation allocates memory.
class InstrumentScope {
 public:
  // Copies the validated source and definitions. Borrows config only during
  // this call. Failure preserves state; configuring an already configured
  // scope returns kInvalidState. Reset is required before reconfiguration.
  [[nodiscard]] base::Status Configure(const MarketDataConfig& config) noexcept;

  // Clears the source binding and definitions, invalidating borrowed entries.
  void Reset() noexcept;

  bool HasInstrument(model::InstrumentId id) const noexcept {
    return FindIndex(id).has_value();
  }

  // Copies the matching definition. An unknown ID leaves out unchanged.
  bool TryGetInstrument(model::InstrumentId id,
                        model::InstrumentDefinition& out) const noexcept;

  // Returns the definition's configuration-order index, or no value for an
  // unknown ID. Indices remain valid until Reset().
  std::optional<std::size_t> FindIndex(model::InstrumentId id) const noexcept;

  // Requires index < size(). The returned definition is borrowed until Reset.
  const model::InstrumentDefinition& operator[](
      std::size_t index) const noexcept {
    return definitions_[index];
  }

  std::size_t size() const noexcept { return instrument_count_; }
  model::SourceId source_id() const noexcept { return source_id_; }

 private:
  // Zero is empty; index_plus_one identifies a configuration-order slot.
  struct LookupSlot {
    model::InstrumentId id;
    std::uint8_t index_plus_one = 0;
  };

  // Keeping at most half the table occupied guarantees an empty probe slot.
  static constexpr std::size_t kLookupCapacity = 2 * kMaxMarketDataInstruments;
  static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);
  static_assert(kMaxMarketDataInstruments <= UINT8_MAX);

  // Returns a table slot; the hash is internal and never persisted.
  static std::size_t Hash(model::InstrumentId id) noexcept;

  // Zero source identity denotes an unconfigured scope.
  model::SourceId source_id_;
  std::size_t instrument_count_ = 0;
  std::array<model::InstrumentDefinition, kMaxMarketDataInstruments>
      definitions_ = {};
  std::array<LookupSlot, kLookupCapacity> lookup_ = {};
};

}  // namespace yquant::md

#endif  // YQUANT_MD_INSTRUMENT_SCOPE_H_
