#include "yquant/trading/trade.h"

#include <cmath>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <string>

namespace yquant::trading {

std::size_t TradeIdHash::operator()(const TradeId& id) const {
  auto hash = std::hash<std::string>{}(id.venue);
  for (const auto* field : {&id.session, &id.value}) {
    hash ^= std::hash<std::string>{}(*field) + 0x9e3779b9U +
            (hash << 6) + (hash >> 2);
  }
  return hash;
}

namespace internal {

bool ValidTrade(const TradeInfo& trade) {
  return !trade.trade_id.venue.empty() && !trade.trade_id.session.empty() &&
         !trade.trade_id.value.empty() && std::isfinite(trade.price) &&
         trade.price > 0 && trade.quantity > 0;
}

}  // namespace internal
}  // namespace yquant::trading
