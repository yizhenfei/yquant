#include "yquant/strategy/strategy_service.h"

#include "yquant/model/quote.h"

namespace yquant::strategy {

void StrategyService::OnQuote(const model::Quote& quote) {
  strategy_.OnQuote(quote);
}

}  // namespace yquant::strategy
