#ifndef YQUANT_MARKET_QUOTE_SOURCE_H_
#define YQUANT_MARKET_QUOTE_SOURCE_H_

#include "yquant/model/quote.h"

namespace yquant::market {

// Indicates whether a quote is available for this read.
enum class QuoteReadResult { kQuote, kIdle };

// Supplies quotes to MarketService. Read must return without waiting for input.
// Implementations may access the output quote only while Read is executing.
class QuoteSource {
 public:
  virtual ~QuoteSource() = default;
  // Writes one available quote and returns kQuote.
  // Returns kIdle if no quote is currently available, leaving quote unchanged.
  virtual QuoteReadResult Read(model::Quote& quote) = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_QUOTE_SOURCE_H_
