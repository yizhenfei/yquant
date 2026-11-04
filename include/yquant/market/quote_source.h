#ifndef YQUANT_MARKET_QUOTE_SOURCE_H_
#define YQUANT_MARKET_QUOTE_SOURCE_H_

#include "yquant/model/quote.h"

namespace yquant::market {

// Provides market quotes to MarketService.
class QuoteSource {
 public:
  virtual ~QuoteSource() = default;

  // Reads one quote into quote and returns true.
  // If no quote can be delivered now, returns false without changing quote.
  // Does not wait for new quotes to arrive.
  // Implementations must not retain a pointer or reference to quote.
  virtual bool TryRead(model::Quote& quote) = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_QUOTE_SOURCE_H_
