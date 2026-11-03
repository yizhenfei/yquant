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
                 std::int32_t& remaining, long double unit_value,
                 PositionSide side, TradeAccounting& result) {
  const auto closed = std::min(quantity, remaining);
  if (closed == 0) return true;
  if (!cost) return false;
  const long double fraction = static_cast<long double>(closed) / quantity;
  const long double allocated_cost = cost->cost * fraction;
  const long double released_margin = cost->margin * fraction;
  result.profit += (unit_value * closed - allocated_cost) *
                   (side == PositionSide::kLong ? 1 : -1);
  result.margin_change -= released_margin;
  quantity -= closed;
  remaining -= closed;
  if (quantity == 0) {
    cost.reset();
  } else if (!AddAmount(cost->cost, -allocated_cost) ||
             !AddAmount(cost->margin, -released_margin) ||
             cost->cost <= 0 || cost->margin < 0) {
    return false;
  }
  return true;
}

// Uses explicit today/yesterday offsets or the configured generic-close rule.
std::optional<TradeAccounting> CalculateTrade(
    const OrderInfo& order, const TradeInfo& trade, const FundsTerms& terms,
    const PositionInfo& current) {
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
  auto remaining = trade.quantity;
  const auto close_today = [&] {
    return CloseBucket(position.today_quantity, position.today_cost, remaining,
                       unit_value, position.key.side, result);
  };
  const auto close_yesterday = [&] {
    return CloseBucket(position.yesterday_quantity, position.yesterday_cost,
                       remaining, unit_value, position.key.side, result);
  };
  bool valid = false;
  if (order.offset == Offset::kCloseToday) {
    valid = close_today();
  } else if (order.offset == Offset::kCloseYesterday) {
    valid = close_yesterday();
  } else if (terms.close_today_first) {
    valid = close_today() && close_yesterday();
  } else {
    valid = close_yesterday() && close_today();
  }
  if (!valid || remaining != 0) return std::nullopt;
  return result;
}

// Selects the holding direction and hedge category affected by an order.
PositionKey HoldingKey(const OrderInfo& order) {
  const bool long_position = (order.side == Side::kBuy) ==
                            (order.offset == Offset::kOpen);
  return {order.instrument_id,
          long_position ? PositionSide::kLong : PositionSide::kShort,
          order.hedge};
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
  const auto key = HoldingKey(order.order);
  const auto row = account.positions.index.find(key);
  const auto position = row == account.positions.index.end()
      ? PositionInfo{key} : account.positions.rows[row->second];
  const auto* terms = FindFundsTerms(account, order.order);
  const auto calculated = terms == nullptr ? std::nullopt
      : CalculateTrade(order.order, trade, *terms, position);
  auto funds = account.funds;
  if (!calculated || !ApplyCash(funds, *calculated)) {
    // Keep the order/trade fact, but do not publish a partly updated ledger.
    account.funds_available = false;
    Diagnose("funds_accounting_unresolved", account.id, order.order.order_id);
    return;
  }
  if (row == account.positions.index.end()) {
    account.positions.index.emplace(key, account.positions.rows.size());
    account.positions.rows.push_back(calculated->position);
  } else {
    account.positions.rows[row->second] = calculated->position;
  }
  account.funds = funds;
  order.booked_quantity += trade.quantity;
}

}  // namespace yquant::trading
