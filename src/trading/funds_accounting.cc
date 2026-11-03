#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

#include "yquant/trading/internal/funds_calculation.h"
#include "yquant/trading/position.h"

namespace yquant::trading {
namespace {

// Holds a tentative fill calculation until every accounting check passes.
struct TradeAccounting {
  PositionInfo position;
  long double margin_change = 0;
  long double profit = 0;
  double fee = 0;
};

// Applies a finite representable delta without discarding a nonzero cash flow.
bool AddAmount(double& value, long double delta) {
  const double next =
      static_cast<double>(static_cast<long double>(value) + delta);
  if (!std::isfinite(next) || (delta != 0 && next == value)) return false;
  value = next;
  return true;
}

// Allocates weighted cost and margin; a final close takes the whole remainder.
bool CloseBucket(std::int32_t& quantity, std::optional<PositionCost>& cost,
                 std::int32_t& frozen, std::int32_t closed,
                 long double unit_value, PositionSide side,
                 TradeAccounting& result) {
  if (closed == 0) return true;
  if (!cost || closed > quantity || closed > frozen) return false;
  const long double fraction = static_cast<long double>(closed) / quantity;
  const long double allocated_cost = cost->cost * fraction;
  const long double released_margin = cost->margin * fraction;
  result.profit += (unit_value * closed - allocated_cost) *
                   (side == PositionSide::kLong ? 1 : -1);
  result.margin_change -= released_margin;
  quantity -= closed;
  frozen -= closed;
  if (quantity == 0) {
    cost.reset();
  } else if (!AddAmount(cost->cost, -allocated_cost) ||
             !AddAmount(cost->margin, -released_margin) ||
             cost->cost <= 0 || cost->margin < 0) {
    return false;
  }
  return true;
}

// Charges the same buckets that were frozen when the close was submitted.
std::optional<TradeAccounting> CalculateTrade(
    const OrderInfo& order, const TradeInfo& trade, const FundsTerms& terms,
    const PositionInfo& current, std::int32_t today_fill) {
  const auto costs = internal::CalculateOrderCost(
      order, terms, trade.price, trade.quantity);
  if (!costs) return std::nullopt;
  TradeAccounting result{current, 0, 0, costs->fee};
  auto& position = result.position;
  const long double unit_value =
      static_cast<long double>(trade.price) * terms.contract_multiplier;
  if (order.offset == Offset::kOpen) {
    if (trade.quantity > std::numeric_limits<std::int32_t>::max() -
                             position.today_quantity) return std::nullopt;
    if (position.today_quantity > 0 && !position.today_cost) {
      return std::nullopt;
    }
    auto cost = position.today_cost.value_or(PositionCost{});
    if (!AddAmount(cost.cost, unit_value * trade.quantity) ||
        !AddAmount(cost.margin, costs->margin)) return std::nullopt;
    position.today_quantity += trade.quantity;
    position.today_cost = cost;
    result.margin_change = costs->margin;
    return result;
  }
  if (!CloseBucket(position.today_quantity, position.today_cost,
                   position.today_frozen, today_fill, unit_value,
                   position.key.side, result) ||
      !CloseBucket(position.yesterday_quantity, position.yesterday_cost,
                   position.yesterday_frozen, trade.quantity - today_fill,
                   unit_value, position.key.side, result)) {
    return std::nullopt;
  }
  return result;
}

// Removes a negative residue only when it is within one floating-point step.
// A larger margin deficit indicates inconsistent accounting and fails booking.
bool ApplyMargin(double& margin, long double change) {
  const long double next = static_cast<long double>(margin) + change;
  const auto upper =
      std::nextafter(margin, std::numeric_limits<double>::infinity());
  if (margin >= 0 && next < 0 && std::isfinite(upper) &&
      static_cast<long double>(upper) + change >= 0) {
    margin = 0;
    return true;
  }
  return AddAmount(margin, change) && margin >= 0;
}

// Checks all balance components before committing a calculated fill.
bool ApplyCash(FundsSnapshot& funds, const TradeAccounting& trade) {
  return AddAmount(funds.funds.balance, trade.profit - trade.fee) &&
         AddAmount(funds.funds.available,
                   trade.profit - trade.fee - trade.margin_change) &&
         ApplyMargin(funds.funds.margin, trade.margin_change) &&
         AddAmount(funds.commission, trade.fee) &&
         AddAmount(funds.realized_pnl, trade.profit);
}

}  // namespace

void TradingService::BookTrade(AccountContext& account, OrderRecord& order,
                               const TradeInfo& trade) {
  if (!account.funds_available) return;
  const auto key = PositionKey::ForOrder(order.order);
  auto index = order.close.row;
  std::int32_t today_fill = 0;
  if (order.order.offset == Offset::kOpen) {
    const auto row = account.positions.index.find(key);
    index = row == account.positions.index.end() ? account.positions.rows.size()
                                                 : row->second;
  } else {
    UpdateCloseHold(account, order);
    if (!account.funds_available) return;
    const auto since_start = order.booked_quantity - order.baseline_booked;
    today_fill = order.close.TodayPrefix(since_start + trade.quantity)
                 - order.close.TodayPrefix(since_start);
  }
  const bool new_position = index == account.positions.rows.size();
  const auto position = new_position ? PositionInfo{key}
                                     : account.positions.rows[index];
  const auto* terms = order.recovered_terms ? order.recovered_terms.get()
                                            : FindFundsTerms(account, order.order);
  const auto calculated = terms == nullptr ? std::nullopt
      : CalculateTrade(order.order, trade, *terms, position, today_fill);
  auto funds = account.funds;
  if (!calculated || !ApplyCash(funds, *calculated)) {
    // Keep the order/trade fact, but do not publish a partly updated ledger.
    account.funds_available = false;
    Diagnose("funds_accounting_unresolved", account.id, order.order.order_id);
    return;
  }
  if (new_position) {
    account.positions.index.emplace(key, account.positions.rows.size());
    account.positions.rows.push_back(calculated->position);
  } else {
    account.positions.rows[index] = calculated->position;
  }
  account.funds = funds;
  order.booked_quantity += trade.quantity;
}

}  // namespace yquant::trading
