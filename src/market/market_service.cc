#include "yquant/market/market_service.h"

#include "yquant/model/quote.h"

namespace yquant::market {

void MarketService::Publish(const model::Quote& quote) {
  if (listener_ != nullptr) {
    listener_->OnQuote(quote);
  }
}

}  // namespace yquant::market
