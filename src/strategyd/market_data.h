#ifndef YQUANT_STRATEGYD_MARKET_DATA_H_
#define YQUANT_STRATEGYD_MARKET_DATA_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <span>
#include <string_view>

#include "yquant/base/monotonic_clock.h"
#include "yquant/market/instrument_scope.h"
#include "yquant/market/market_data_config.h"
#include "yquant/strategy/callback_phase.h"
#include "yquant/strategy/market_data.h"

namespace yquant::strategyd {

// Implements strategy lookups, startup subscriptions, and last-dispatched quote
// snapshots for a configured instrument scope. The runner configures the scope,
// brackets callbacks, and applies quotes before invoking OnTick.
//
// All operations are serialized on the owning strategy thread. Subscription
// and snapshot storage remains fixed during subscription and quote processing.
class MarketDataImpl final : public strategy::MarketData {
 public:
  MarketDataImpl(const MarketDataImpl&) = delete;
  MarketDataImpl& operator=(const MarketDataImpl&) = delete;

  // Borrows clock until destruction to reject quotes with future receive times.
  explicit MarketDataImpl(const base::MonotonicClock& clock) noexcept
      : clock_(clock) {}

  // Copies source identity and instrument metadata into an unconfigured view.
  // Borrows config only for this call. Returns kInvalidState during a callback
  // or if already configured; all failures preserve the current configuration.
  [[nodiscard]] base::Status Configure(
      const market::MarketDataConfig& config) noexcept;
  // Clears configuration, snapshots, subscriptions, and callback phase. Call
  // outside callbacks after the strategy has finished using the view.
  void Reset() noexcept;

  // Enters phase before invoking a strategy callback. Requires no active
  // callback; pair with EndCallback even when the callback returns an error.
  void BeginCallback(strategy::CallbackPhase phase) noexcept { phase_ = phase; }
  // Ends the current callback phase, closing startup subscription registration.
  void EndCallback() noexcept { phase_ = strategy::CallbackPhase::kNone; }

  // Resolves canonical venue/symbol pairs within the configured scope,
  // including futures symbols. Failure leaves out unchanged.
  [[nodiscard]] base::ErrorCode FindInstrument(
      std::string_view venue, std::string_view symbol,
      model::InstrumentDefinition& out) const noexcept override;
  [[nodiscard]] base::ErrorCode SubscribeQuotes(
      model::InstrumentId id) noexcept override;
  [[nodiscard]] bool TryGetLatestQuote(
      model::InstrumentId id, model::QuoteTick& out) const noexcept override;

  // Returns a view of registered IDs, stable after OnStart until Reset or
  // destruction. During OnStart, later subscriptions may extend the list.
  std::span<const model::InstrumentId> subscriptions() const noexcept {
    return {subscriptions_.data(), subscription_count_};
  }

  // Validates a subscribed quote's identity, fields, receive time, and units.
  // Copies a newer quote into its snapshot and sets accepted=true; a valid old
  // or duplicate quote sets accepted=false without changing the snapshot.
  // Failure preserves snapshots and accepted. Borrows tick only for this call.
  [[nodiscard]] base::ErrorCode ApplyQuote(const model::QuoteTick& tick,
                                           bool& accepted) noexcept;

 private:
  // Tracks one configured instrument; latest is meaningful only when has_quote
  // is true. Subscription order is stored separately from scope order.
  struct InstrumentSlot {
    model::QuoteTick latest;
    bool subscribed = false;
    bool has_quote = false;
  };
  const base::MonotonicClock& clock_;
  market::InstrumentScope instruments_;
  std::array<InstrumentSlot, market::kMaxMarketDataInstruments> snapshots_ = {};
  std::array<model::InstrumentId, market::kMaxMarketDataInstruments>
      subscriptions_ = {};
  size_t subscription_count_ = 0;  // Populated prefix of subscriptions_.
  strategy::CallbackPhase phase_ = strategy::CallbackPhase::kNone;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_H_
