#ifndef YQUANT_MARKET_MARKET_CLIENT_H_
#define YQUANT_MARKET_MARKET_CLIENT_H_

#include <cstdint>

#include "yquant/market/market_service.h"

namespace yquant::market {

// Subscribes to market quotes for one fixed callback receiver.
class MarketClient {
 public:
  virtual ~MarketClient() = default;
  // Binds the only callback receiver. Returns false if already bound or closed.
  virtual bool BindListener(QuoteListener& listener) = 0;
  // Returns false if unbound, closed or subscriptions cannot change during delivery.
  virtual bool Subscribe(std::uint32_t instrument_id) = 0;
  virtual bool Unsubscribe(std::uint32_t instrument_id) = 0;
  // Reports whether Close can complete now without interrupting a callback.
  virtual bool CanClose() const = 0;
  // Removes all subscriptions. Returns false if cleanup would interrupt delivery.
  // Repeated Close succeeds. Destroy the receiver only after Close succeeds.
  virtual bool Close() = 0;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_CLIENT_H_
