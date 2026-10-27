#ifndef YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
#define YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_

#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"

namespace yquant::market {

// Connects a consumer to a same-process service through direct method calls.
// The service is borrowed and must outlive this client. Listener lifetime
// follows the service's binding; destroying the client does not remove it.
class LocalMarketClient final : public MarketClient {
 public:
  explicit LocalMarketClient(MarketService& service) : service_(service) {}
  LocalMarketClient(const LocalMarketClient&) = delete;
  LocalMarketClient& operator=(const LocalMarketClient&) = delete;

  void Subscribe(QuoteListener& listener) override {
    service_.Subscribe(listener);
  }

 private:
  MarketService& service_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
