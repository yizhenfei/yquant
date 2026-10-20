#ifndef YQUANT_STRATEGY_MARKET_DATA_H_
#define YQUANT_STRATEGY_MARKET_DATA_H_

#include <string_view>

#include "yquant/base/status.h"
#include "yquant/model/instrument.h"
#include "yquant/model/quote_tick.h"

namespace yquant::strategy {

// Host-owned market-data service bound to the source selected by host config.
// Used only by the currently executing strategy callback on its owning thread.
// Do not retain this object or pass it to another thread. Instrument metadata
// and last dispatched snapshots remain available during OnStop; returned
// values own their data.
class MarketData {
 public:
  virtual ~MarketData() noexcept = default;

  // Copies a definition identified by venue and six-digit symbol. Input views
  // are borrowed for this call only. Returns kInvalidArgument and leaves out
  // unchanged if no definition matches. Intended for initialization lookups.
  [[nodiscard]] virtual base::ErrorCode FindInstrument(
      std::string_view venue, std::string_view symbol,
      model::InstrumentDefinition& out) const noexcept = 0;

  // Valid only in OnStart. Registers an instrument with the host-bound source;
  // success does not imply that a quote is available. Repeating a registered
  // instrument is successful. Unknown IDs return kInvalidArgument, an absent
  // source returns kUnsupported, and other phases return kInvalidState.
  // Failures preserve all existing registrations. Dispatch begins only after
  // OnStart succeeds; startup failure removes this instance's registrations.
  [[nodiscard]] virtual base::ErrorCode SubscribeQuotes(
      model::InstrumentId id) noexcept = 0;

  // Copies the instrument's last dispatched snapshot from the host-bound
  // source, without waiting or expiring it. Returns false without changing out
  // for an absent source or invalid, unsubscribed, or unseen IDs. Within
  // OnTick, querying its instrument returns the exact callback value.
  [[nodiscard]] virtual bool TryGetLatestQuote(
      model::InstrumentId id, model::QuoteTick& out) const noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_MARKET_DATA_H_
