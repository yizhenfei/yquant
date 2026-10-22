#ifndef YQUANT_STRATEGYD_MARKET_DATA_H_
#define YQUANT_STRATEGYD_MARKET_DATA_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/md/market_data_config.h"
#include "yquant/strategy/callback_phase.h"
#include "yquant/strategy/market_data.h"

namespace yquant::strategyd {

// Main-thread strategy subscriptions and last dispatched snapshots, indexed by
// a fixed instrument scope. No map or buffer grows at runtime.
class MarketDataImpl final : public strategy::MarketData {
 public:
  // Borrows clock for this object's entire lifetime; all access is serialized
  // by the owning strategy runner thread. Does not acquire other resources.
  explicit MarketDataImpl(const base::MonotonicClock& clock) noexcept
      : clock_(clock) {}

  // Binds the configured source and copies its validated instrument metadata
  // into an unconfigured object outside callbacks.
  // Borrows config only for this call. Failure preserves the current state;
  // successful configuration remains in place until Reset().
  [[nodiscard]] base::Status Configure(
      const md::MarketDataConfig& config) noexcept;
  // Clears the source binding, cached snapshots, and registrations. Call only
  // outside callbacks.
  void Reset() noexcept;

  // Host-only, non-nested phase boundaries on the owning thread. Begin before
  // entering a strategy callback and End after it returns, including on error.
  void BeginCallback(strategy::CallbackPhase phase) noexcept { phase_ = phase; }
  void EndCallback() noexcept { phase_ = strategy::CallbackPhase::kNone; }

  // Borrows the configured scope for host services until Reset().
  const md::InstrumentScope& instruments() const noexcept {
    return instruments_;
  }

  [[nodiscard]] base::ErrorCode FindInstrument(
      std::string_view venue, std::string_view symbol,
      model::InstrumentDefinition& out) const noexcept override;
  [[nodiscard]] base::ErrorCode SubscribeQuotes(
      model::InstrumentId id) noexcept override;
  [[nodiscard]] bool TryGetLatestQuote(
      model::InstrumentId id, model::QuoteTick& out) const noexcept override;

  // Borrows fixed storage until Reset(). The runner consumes the registration
  // list after OnStart has returned, when no further registrations are allowed.
  std::span<const model::InstrumentId> subscriptions() const noexcept {
    return {subscriptions_.data(), subscription_count_};
  }

  // Validates against the bound source and instrument tick/quantity units.
  // Accepted quotes update the cache; old quotes succeed with accepted=false.
  // Failure changes neither cache nor accepted.
  [[nodiscard]] base::ErrorCode ApplyQuote(const model::QuoteTick& tick,
                                           bool& accepted) noexcept;

 private:
  // latest is readable only once has_quote is true for this subscribed slot.
  struct InstrumentSlot {
    model::QuoteTick latest;
    bool subscribed = false;
    bool has_quote = false;
  };
  const base::MonotonicClock& clock_;
  md::InstrumentScope instruments_;
  std::array<InstrumentSlot, md::kMaxMarketDataInstruments> snapshots_ = {};
  std::array<model::InstrumentId, md::kMaxMarketDataInstruments>
      subscriptions_ = {};
  std::size_t subscription_count_ = 0;
  strategy::CallbackPhase phase_ = strategy::CallbackPhase::kNone;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_H_
