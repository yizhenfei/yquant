#ifndef YQUANT_TRADING_SIMNOW_ORDER_HISTORY_H_
#define YQUANT_TRADING_SIMNOW_ORDER_HISTORY_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "yquant/trading/order.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

// Identifies the caller order and strategy encoded in a YQuant order tag.
struct SimNowOrderOwner {
  OrderId order_id;
  std::uint64_t strategy_id = 0;
  bool operator==(const SimNowOrderOwner&) const = default;
};

// Preserves the counter's order fields for this account and trading day.
// A missing owner leaves the order accessible by its native identifiers.
struct SimNowOrder {
  int front = 0, session = 0;
  std::string ref, exchange, system_id, symbol;
  char direction = 0, offset = 0, hedge = 0;
  char status = 0, submit_status = 0, price_type = 0;
  double price = 0;
  int quantity = 0, traded = 0, remaining = 0;
  std::optional<SimNowOrderOwner> owner;
  bool operator==(const SimNowOrder&) const = default;
};

// Identifies a fill's order by index in the same history, including unowned
// orders. The index stays valid as more orders are appended.
struct SimNowFill {
  std::size_t order_index;
  double price;
  int quantity;
  bool operator==(const SimNowFill&) const = default;
};

// Builds an account's trading-day history before the snapshot is published.
// Identical duplicates succeed. Conflicting identifiers or data fail the
// collection. Neither insertion sends a request or changes any account funds.
class SimNowOrderHistory {
 public:
  // Indexes the order by each available native key and its decoded OrderId.
  bool AddOrder(SimNowOrder order);
  // Looks up the order by exchange and system_id, checks its trade fields,
  // and records this execution once. An unknown order returns false.
  bool AddFill(TradeId id, const std::string& system_id,
               const std::string& symbol, char direction, char offset,
               char hedge, double price, int quantity);
  const std::vector<SimNowOrder>& orders() const { return orders_; }
  const auto& fills() const { return fills_; }
  // Returns the largest recovered sequence for use with OrderIdGenerator.
  // Take the maximum across this caller's accounts before allocating IDs.
  std::uint64_t LastSequence(std::uint64_t caller) const;
  // The returned order is borrowed until the next insertion or destruction.
  const SimNowOrder* Find(OrderId id) const;
  const SimNowOrder* Find(int front, int session, const std::string& ref) const;
  const SimNowOrder* Find(const std::string& exchange,
                          const std::string& system_id) const;

 private:
  using NativeIndex = std::unordered_map<std::string, std::size_t>;
  std::vector<SimNowOrder> orders_;
  std::unordered_map<OrderId, std::size_t, OrderIdHash> owned_;
  std::unordered_map<std::uint64_t, NativeIndex> sessions_;
  std::unordered_map<std::string, NativeIndex> exchange_orders_;
  std::unordered_map<TradeId, SimNowFill, TradeIdHash> fills_;
  std::vector<int> detailed_quantity_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMNOW_ORDER_HISTORY_H_
