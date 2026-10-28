#include "yquant/market/market_service.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "yquant/model/quote.h"

namespace yquant::market {

void MarketService::Subscribe(std::uint32_t instrument_id,
                              QuoteListener& listener) {
  const auto matches = [instrument_id, &listener](const Subscription& entry) {
    return entry.instrument_id == instrument_id && entry.listener == &listener;
  };
  if (!std::any_of(subscriptions_.begin(), subscriptions_.end(), matches)) {
    subscriptions_.push_back({instrument_id, &listener});
  }
}

void MarketService::Unsubscribe(std::uint32_t instrument_id,
                                QuoteListener& listener) {
  std::erase_if(subscriptions_,
                [instrument_id, &listener](const Subscription& entry) {
                  return entry.instrument_id == instrument_id &&
                         entry.listener == &listener;
                });
}

void MarketService::Publish(const model::Quote& quote) {
  for (const Subscription& entry : subscriptions_) {
    if (entry.instrument_id == quote.instrument_id) {
      entry.listener->OnQuote(quote);
    }
  }
}

}  // namespace yquant::market
