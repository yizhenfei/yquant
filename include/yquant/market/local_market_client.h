#ifndef YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
#define YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_

#include <cstdint>

#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"

namespace yquant::market {

// Forwards subscription changes directly to a same-process service.
// The service must outlive this client. Destroying the client leaves its
// subscriptions active; remove them before destroying the listeners.
class LocalMarketClient final : public MarketClient {
 public:
  explicit LocalMarketClient(MarketService& service) : service_(service) {}
  LocalMarketClient(const LocalMarketClient&) = delete;
  LocalMarketClient& operator=(const LocalMarketClient&) = delete;

  // MarketClient implementation.
  void Subscribe(std::uint32_t instrument_id,
                 QuoteListener& listener) override {
    service_.Subscribe(instrument_id, listener);
  }

  void Unsubscribe(std::uint32_t instrument_id,
                   QuoteListener& listener) override {
    service_.Unsubscribe(instrument_id, listener);
  }

 private:
  MarketService& service_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
