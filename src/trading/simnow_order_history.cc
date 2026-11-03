#include "yquant/trading/simnow_order_history.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace yquant::trading {
namespace {

// Packs the two signed 32-bit CTP identifiers without losing their bit patterns.
std::uint64_t SessionKey(int front, int session) {
  return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(front)) << 32) |
         static_cast<std::uint32_t>(session);
}

}  // namespace

const SimNowOrder* SimNowOrderHistory::Find(OrderId id) const {
  const auto it = owned_.find(id);
  return it == owned_.end() ? nullptr : &orders_[it->second];
}

const SimNowOrder* SimNowOrderHistory::Find(
    int front, int session, const std::string& ref) const {
  const auto scope = sessions_.find(SessionKey(front, session));
  if (scope == sessions_.end()) return nullptr;
  const auto it = scope->second.find(ref);
  return it == scope->second.end() ? nullptr : &orders_[it->second];
}

const SimNowOrder* SimNowOrderHistory::Find(
    const std::string& exchange, const std::string& system_id) const {
  const auto scope = exchange_orders_.find(exchange);
  if (scope == exchange_orders_.end()) return nullptr;
  const auto it = scope->second.find(system_id);
  return it == scope->second.end() ? nullptr : &orders_[it->second];
}

bool SimNowOrderHistory::AddOrder(SimNowOrder order) {
  const bool has_ref = !order.ref.empty();
  const bool has_system = !order.system_id.empty() && !order.exchange.empty();
  if ((!has_ref && !has_system) || order.symbol.empty() ||
      order.quantity <= 0 || order.traded < 0 || order.traded > order.quantity ||
      order.remaining < 0 || order.remaining > order.quantity - order.traded ||
      !std::isfinite(order.price) || (order.owner && !order.owner->order_id.sequence)) {
    return false;
  }
  if (auto* old = Find(order.front, order.session, order.ref)) return *old == order;
  if (auto* old = Find(order.exchange, order.system_id)) return *old == order;
  if (order.owner && !owned_.emplace(order.owner->order_id, orders_.size()).second) {
    return false;
  }
  if (has_ref) {
    sessions_[SessionKey(order.front, order.session)].emplace(order.ref, orders_.size());
  }
  if (has_system) {
    exchange_orders_[order.exchange].emplace(order.system_id, orders_.size());
  }
  detailed_quantity_.push_back(0);
  orders_.push_back(std::move(order));
  return true;
}

bool SimNowOrderHistory::AddFill(TradeId id, const std::string& system_id,
                                 const std::string& symbol, char direction,
                                 char offset, char hedge, double price,
                                 int quantity) {
  const auto* source = Find(id.venue, system_id);
  if (!source || id.session.empty() || id.value.empty() ||
      quantity <= 0 || !(price > 0 && price < std::numeric_limits<double>::max())) {
    return false;
  }
  if (source->symbol != symbol || source->direction != direction ||
      source->offset != offset || source->hedge != hedge) return false;
  const auto index = static_cast<std::size_t>(source - orders_.data());
  SimNowFill fill{index, price, quantity};
  const auto previous = fills_.find(id);
  if (previous != fills_.end()) return previous->second == fill;
  auto& filled = detailed_quantity_[index];
  if (quantity > source->quantity - filled) return false;
  fills_.emplace(std::move(id), fill);
  filled += quantity;
  return true;
}

std::uint64_t SimNowOrderHistory::LastSequence(std::uint64_t caller) const {
  std::uint64_t last = 0;
  for (const auto& [id, index] : owned_) {
    if (id.caller_service_id == caller) last = std::max(last, id.sequence);
  }
  return last;
}

}  // namespace yquant::trading
