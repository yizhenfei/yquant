#ifndef YQUANT_MARKET_MARKET_CLIENT_H_
#define YQUANT_MARKET_MARKET_CLIENT_H_

#include <cstdint>

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

// Selects the instruments a listener receives, regardless of service location.
// Keep each listener alive until all its subscriptions have been removed.
// Change subscriptions between quote deliveries, outside OnQuote callbacks.
class MarketClient {
 public:
  virtual ~MarketClient() = default;
  // Subscribes listener to instrument_id. Repeating this pair has no effect.
  virtual void Subscribe(std::uint32_t instrument_id,
                         QuoteListener& listener) = 0;
  // Stops sending instrument_id to listener. Does nothing if not subscribed.
  virtual void Unsubscribe(std::uint32_t instrument_id,
                           QuoteListener& listener) = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_CLIENT_H_
