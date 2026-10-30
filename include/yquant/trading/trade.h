#ifndef YQUANT_TRADING_TRADE_H_
#define YQUANT_TRADING_TRADE_H_

#include <cstddef>
#include <cstdint>
#include <string>

#include "yquant/trading/order.h"

namespace yquant::trading {

// Preserves the counter's complete trade identity within one account.
// venue identifies the counter's market; session scopes ID reuse (for example,
// a trading day). Adapters preserve all three fields without truncation.
struct TradeId {
  std::string venue;
  std::string session;
  std::string value;
  bool operator==(const TradeId&) const = default;
};

// Hashes every identity field; equality still compares the complete values.
struct TradeIdHash {
  std::size_t operator()(const TradeId& id) const;
};

// Owns one fill's identity, original order ID, execution price and quantity.
// The account comes from the adapter binding, not an untrusted report field.
struct TradeInfo {
  OrderId order_id;
  TradeId trade_id;
  double price = 0;
  std::int32_t quantity = 0;
  bool operator==(const TradeInfo&) const = default;
};

namespace internal {

// Validates a trade at the adapter boundary before storing or applying it.
bool ValidTrade(const TradeInfo& trade);

}  // namespace internal
}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADE_H_
