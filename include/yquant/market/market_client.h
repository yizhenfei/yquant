#ifndef YQUANT_MARKET_MARKET_CLIENT_H_
#define YQUANT_MARKET_MARKET_CLIENT_H_

#include <cstdint>

#include "yquant/market/market_service.h"

namespace yquant::market {

// Provides an interface for subscribing to market quotes.
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
