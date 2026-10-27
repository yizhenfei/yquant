#ifndef YQUANT_MARKET_MARKET_CLIENT_H_
#define YQUANT_MARKET_MARKET_CLIENT_H_

#include "yquant/model/quote.h"

namespace yquant::market {

// Receives subscribed quote updates. Implementations copy any data needed after
// the callback returns; the producer retains ownership of the input.
class QuoteListener {
 public:
  virtual ~QuoteListener() = default;
  // The quote is borrowed until OnQuote returns.
  virtual void OnQuote(const model::Quote& quote) = 0;
};

// Strategy-facing interface for binding a quote consumer. Callers use this
// interface independently of the concrete service's deployment location.
class MarketClient {
 public:
  virtual ~MarketClient() = default;
  // Binds a borrowed listener that must outlive subsequent deliveries.
  virtual void Subscribe(QuoteListener& listener) = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_CLIENT_H_
