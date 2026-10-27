#ifndef YQUANT_MARKET_MARKET_SERVICE_H_
#define YQUANT_MARKET_MARKET_SERVICE_H_

#include "yquant/market/market_client.h"
#include "yquant/model/quote.h"

namespace yquant::market {

// Forwards source quotes synchronously to one borrowed listener. Configure the
// listener before publishing and keep it alive through the final delivery.
class MarketService {
 public:
  MarketService() = default;
  MarketService(const MarketService&) = delete;
  MarketService& operator=(const MarketService&) = delete;

  // Replaces the current listener binding without taking ownership.
  void Subscribe(QuoteListener& listener) { listener_ = &listener; }
  // Delivers the borrowed quote before returning; skips unbound deliveries.
  void Publish(const model::Quote& quote);

 private:
  QuoteListener* listener_ = nullptr;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_H_
