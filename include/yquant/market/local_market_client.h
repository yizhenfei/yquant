#ifndef YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
#define YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_

#include <cstdint>

#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"

namespace yquant::market {

// Connects one receiver to a local market service. Closing this client removes
// only its subscriptions, even when other clients use the same receiver.
// Destroy between service calls; the service must still be available.
class LocalMarketClient final : public MarketClient, private QuoteListener {
 public:
  explicit LocalMarketClient(MarketService& service) : service_(service) {}
  ~LocalMarketClient() override;
  LocalMarketClient(const LocalMarketClient&) = delete;
  LocalMarketClient& operator=(const LocalMarketClient&) = delete;

  // MarketClient implementation.
  bool BindListener(QuoteListener& listener) override;
  bool Subscribe(std::uint32_t instrument_id) override;
  bool Unsubscribe(std::uint32_t instrument_id) override;
  bool CanClose() const override;
  bool Close() override;

 private:
  // QuoteListener implementation.
  void OnQuote(const model::Quote& quote) override;

  MarketService& service_;
  QuoteListener* listener_ = nullptr;
  bool closed_ = false;
  bool registered_ = false;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_LOCAL_MARKET_CLIENT_H_
