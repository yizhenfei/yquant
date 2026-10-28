#ifndef YQUANT_MARKET_MARKET_SERVICE_H_
#define YQUANT_MARKET_MARKET_SERVICE_H_

#include <cstdint>
#include <vector>

#include "yquant/market/market_client.h"
#include "yquant/model/quote.h"

namespace yquant::market {

// Stores subscriptions by instrument and listener. Publish calls each matching
// listener once before returning. Change subscriptions between Publish calls;
// listeners must remain alive until their subscriptions are removed.
class MarketService {
 public:
  MarketService() = default;
  MarketService(const MarketService&) = delete;
  MarketService& operator=(const MarketService&) = delete;

  // Subscribes listener to instrument_id; duplicate requests have no effect.
  void Subscribe(std::uint32_t instrument_id, QuoteListener& listener);
  // Removes listener's subscription to instrument_id if one exists.
  void Unsubscribe(std::uint32_t instrument_id, QuoteListener& listener);
  // Delivers quote to listeners subscribed to its instrument_id.
  // Does nothing when there are no matching subscriptions.
  void Publish(const model::Quote& quote);

 private:
  // Identifies one listener's subscription to one instrument.
  struct Subscription {
    std::uint32_t instrument_id;
    QuoteListener* listener;
  };
  std::vector<Subscription> subscriptions_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_H_
