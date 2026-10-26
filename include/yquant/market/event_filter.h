#ifndef YQUANT_MARKET_EVENT_FILTER_H_
#define YQUANT_MARKET_EVENT_FILTER_H_

#include <stdint.h>

#include <algorithm>
#include <span>

#include "yquant/market/event.h"

namespace yquant::market {

// Receiver-side event selection, independent of transport and cursor state.
// Lists are borrowed and need not be sorted. Their storage must remain valid
// and unmodified during Matches; callers choose their own storage capacity.
// An unrestricted dimension accepts every value; a restricted empty list
// accepts none. Stream status always passes; instrument status bypasses types.
struct EventFilter {
  std::span<const uint64_t> instruments;
  std::span<const uint32_t> event_types;
  bool all_instruments = true;
  bool all_event_types = true;

  // Pure header matching; does not allocate or change the event or filter.
  bool Matches(const EventHeader& header) const noexcept {
    if (header.scope == EventScope::kStreamStatus) {
      return true;
    }
    if (!all_instruments &&
        std::find(instruments.begin(), instruments.end(),
                  header.instrument_id) == instruments.end()) {
      return false;
    }
    return header.scope == EventScope::kInstrumentStatus || all_event_types ||
           std::find(event_types.begin(), event_types.end(), header.type) !=
               event_types.end();
  }
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_EVENT_FILTER_H_
