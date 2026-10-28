#ifndef YQUANT_MARKET_MARKET_SERVICE_H_
#define YQUANT_MARKET_MARKET_SERVICE_H_

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "yquant/container/service.h"
#include "yquant/market/market_client.h"
#include "yquant/market/quote_source.h"
#include "yquant/model/quote.h"

namespace yquant::market {

// Receives quotes from a source and delivers them to subscribed listeners.
// The source is not owned; keep it alive until this service is destroyed.
// Change subscriptions between Run calls. Keep listeners alive until removed.
class MarketService final : public container::Service {
 public:
  explicit MarketService(QuoteSource& source) : source_(source) {}
  MarketService(const MarketService&) = delete;
  MarketService& operator=(const MarketService&) = delete;

  // Subscribes listener to instrument_id; duplicate requests have no effect.
  void Subscribe(std::uint32_t instrument_id, QuoteListener& listener);
  // Removes listener's subscription to instrument_id if one exists.
  void Unsubscribe(std::uint32_t instrument_id, QuoteListener& listener);
  // Delivers available quotes until the source is idle or the batch limit
  // is reached, then returns so other services can run.
  // If Run is already active, returns without reading again.
  void Run() override;

 private:
  // Delivers quote only to listeners subscribed to its instrument.
  void Dispatch(const model::Quote& quote);

  QuoteSource& source_;
  bool running_ = false;
  std::unordered_map<std::uint32_t, std::vector<QuoteListener*>> subscriptions_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_H_
