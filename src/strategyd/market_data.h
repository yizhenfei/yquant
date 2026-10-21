#ifndef YQUANT_STRATEGYD_MARKET_DATA_H_
#define YQUANT_STRATEGYD_MARKET_DATA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "strategyd/market_data_config.h"
#include "strategyd/monotonic_clock.h"
#include "yquant/strategy/market_data.h"

namespace yquant::strategyd {

// Host-owned callback phase controls when subscription registration is legal.
enum class CallbackPhase {
  kNone,
  kStart,
  kTick,
  kOrderUpdate,
  kFill,
  kTimer,
  kStop
};

// Main-thread dictionary, subscriptions, and last dispatched snapshots. Stable
// InstrumentIds map to private dense slots; no map or buffer grows at runtime.
class MarketDataImpl final : public strategy::MarketData {
 public:
  // Borrows clock for this object's entire lifetime; all access is serialized
  // by the owning strategy runner thread. Does not acquire other resources.
  explicit MarketDataImpl(const MonotonicClock& clock) noexcept
      : clock_(clock) {}

  // Copies validated metadata into an unconfigured object outside callbacks.
  // Borrows config only for this call. Failure preserves the current state;
  // successful configuration remains in place until Reset().
  [[nodiscard]] base::Status Configure(const MarketDataConfig& config) noexcept;
  // Invalidates cached snapshots and registrations. Call only outside
  // callbacks.
  void Reset() noexcept;

  // Host-only, non-nested phase boundaries on the owning thread. Begin before
  // entering a strategy callback and End after it returns, including on error.
  void BeginCallback(CallbackPhase phase) noexcept { phase_ = phase; }
  void EndCallback() noexcept { phase_ = CallbackPhase::kNone; }

  // Checks the shared immutable dictionary without requiring a subscription.
  bool HasInstrument(model::InstrumentId id) const noexcept {
    return FindIndex(id).has_value();
  }

  // Copies shared metadata for host accounting, independently of subscription.
  // Unknown IDs leave out unchanged.
  bool TryGetInstrument(model::InstrumentId id,
                        model::InstrumentDefinition& out) const noexcept {
    const auto index = FindIndex(id);
    if (!index) return false;
    out = instruments_[*index].definition;
    return true;
  }

  [[nodiscard]] base::ErrorCode FindInstrument(
      std::string_view venue, std::string_view symbol,
      model::InstrumentDefinition& out) const noexcept override;
  [[nodiscard]] base::ErrorCode SubscribeQuotes(
      model::SourceId source_id, model::InstrumentId id) noexcept override;
  [[nodiscard]] bool TryGetLatestQuote(
      model::SourceId source_id, model::InstrumentId id,
      model::QuoteTick& out) const noexcept override;

  // Borrows fixed storage until Reset(). The runner consumes the registration
  // list after OnStart has returned, when no further registrations are allowed.
  std::span<const model::InstrumentId> subscriptions() const noexcept {
    return {subscriptions_.data(), subscription_count_};
  }

  // Validates structure and instrument tick/quantity units before publishing.
  // Accepted quotes update the cache; old quotes
  // succeed with accepted=false. Failure changes neither cache nor accepted.
  [[nodiscard]] base::ErrorCode ApplyQuote(const model::QuoteTick& tick,
                                           bool& accepted) noexcept;

 private:
  // latest is readable only once has_quote is true for this subscribed slot.
  struct InstrumentSlot {
    model::InstrumentDefinition definition;
    model::QuoteTick latest;
    bool subscribed = false;
    bool has_quote = false;
  };
  // Zero is empty; index_plus_one maps a stable identity to a dense array slot.
  struct LookupSlot {
    model::InstrumentId id;
    std::uint8_t index_plus_one = 0;
  };

  static constexpr std::size_t kLookupCapacity = 128;
  // Hashes an identity to the fixed lookup table; never persists this hash.
  static std::size_t Hash(model::InstrumentId id) noexcept;
  // Returns no index for unknown IDs; reads only the initialization-time table.
  std::optional<std::size_t> FindIndex(model::InstrumentId id) const noexcept;

  const MonotonicClock& clock_;
  model::SourceId source_id_;
  std::size_t instrument_count_ = 0;
  std::array<InstrumentSlot, kMaxMarketDataInstruments> instruments_ = {};
  std::array<LookupSlot, kLookupCapacity> lookup_ = {};
  std::array<model::InstrumentId, kMaxMarketDataInstruments> subscriptions_ =
      {};
  std::size_t subscription_count_ = 0;
  CallbackPhase phase_ = CallbackPhase::kNone;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_H_
