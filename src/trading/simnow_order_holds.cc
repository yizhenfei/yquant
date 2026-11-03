#include "simnow_order_holds.h"

#include <ThostFtdcTraderApi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

#include "yquant/trading/internal/funds_calculation.h"

namespace yquant::trading::internal {
namespace {

// Bounds total frozen today lots using each counter row's remaining positions.
// A mixed-date row can constrain the split without always determining it.
struct PositionFreeze {
  std::int64_t total = 0, min_today = 0, max_today = 0;
  std::int64_t explicit_today = 0, explicit_yesterday = 0, generic = 0;
  std::vector<std::size_t> generic_holds;
};

using PositionScope = std::tuple<std::string, std::string, std::string, char, char>;
using Positions = std::map<PositionScope, PositionFreeze>;

std::string CollectFreezes(const std::vector<SimNowPosition>& positions,
                           Positions& groups) {
  for (const auto& p : positions) {
    if (!p.long_frozen && !p.short_frozen) continue;
    const bool long_side = p.direction == THOST_FTDC_PD_Long;
    if ((!long_side && p.direction != THOST_FTDC_PD_Short) ||
        (long_side ? p.long_frozen : p.short_frozen) != 0) {
      return "unresolved_position_direction " + p.symbol;
    }
    const int frozen = long_side ? p.short_frozen : p.long_frozen;
    if (frozen > p.quantity) return "excess_position_freeze " + p.symbol;
    auto& group = groups[{p.exchange, p.symbol, p.invest_unit, p.hedge, p.direction}];
    group.total += frozen;
    group.min_today += std::max(0, frozen - p.yesterday_quantity());
    group.max_today += std::min(frozen, p.today);
  }
  return {};
}

void AddClose(const SimNowOrder& order, std::vector<SimNowOrderHold>& holds,
              Positions& groups) {
  auto& hold = holds.back();
  const auto side = order.direction == THOST_FTDC_D_Sell ? THOST_FTDC_PD_Long
                                                         : THOST_FTDC_PD_Short;
  auto& group = groups[{order.exchange, order.symbol, order.invest_unit, order.hedge, side}];
  if (order.offset == THOST_FTDC_OF_CloseToday) {
    hold.today = hold.remaining;
    group.explicit_today += hold.today;
  } else if (order.offset == THOST_FTDC_OF_CloseYesterday) {
    hold.yesterday = hold.remaining;
    group.explicit_yesterday += hold.yesterday;
  } else {
    group.generic += hold.remaining;
    group.generic_holds.push_back(holds.size() - 1);
  }
}

std::string AssignBuckets(const Positions& groups, std::vector<SimNowOrderHold>& holds) {
  for (const auto& [scope, g] : groups) {
    const auto& symbol = std::get<1>(scope);
    const auto low = std::max<std::int64_t>(0, g.min_today - g.explicit_today);
    const auto high = std::min(g.generic, g.max_today - g.explicit_today);
    if (g.explicit_today + g.explicit_yesterday + g.generic != g.total || low > high) {
      return "position_freeze_mismatch " + symbol;
    }
    if (low != high || (g.generic_holds.size() > 1 && low > 0 && low < g.generic)) {
      return "ambiguous_close_allocation " + symbol;
    }
    for (const auto index : g.generic_holds) {
      auto& hold = holds[index];
      hold.today = low == g.generic ? hold.remaining : static_cast<int>(low);
      hold.yesterday = hold.remaining - hold.today;
    }
  }
  return {};
}

bool Working(const SimNowOrder& order) {
  return order.status == THOST_FTDC_OST_NoTradeQueueing ||
         order.status == THOST_FTDC_OST_PartTradedQueueing;
}

OrderRateKey RateKey(const SimNowOrder& order) {
  return {order.exchange, order.symbol, order.invest_unit, order.hedge};
}

std::optional<Offset> OrderOffset(char offset) {
  switch (offset) {
    case THOST_FTDC_OF_Open: return Offset::kOpen;
    case THOST_FTDC_OF_Close: return Offset::kClose;
    case THOST_FTDC_OF_CloseToday: return Offset::kCloseToday;
    case THOST_FTDC_OF_CloseYesterday: return Offset::kCloseYesterday;
    default: return std::nullopt;
  }
}

// Accumulates the two independently reported cash holds for one currency.
struct FundsFreeze {
  long double margin = 0;
  long double commission = 0;
};

// Compare totals at sub-cent precision; a nonzero hold must never match zero.
bool SameMoney(long double calculated, double reported) {
  return (calculated == 0) == (reported == 0) &&
         std::abs(calculated - reported) < 0.005L;
}

std::string Associate(const SimNowStartupSnapshot& snapshot, const OrderRates& rates,
                      std::vector<SimNowOrderHold>& holds) {
  Positions positions;
  if (auto error = CollectFreezes(snapshot.positions, positions); !error.empty()) return error;
  std::map<std::string_view, FundsFreeze> funds;
  for (const auto& b : snapshot.balances) funds.emplace(b.currency.code(), FundsFreeze{});
  const auto& orders = snapshot.history.orders();
  for (std::size_t i = 0; i < orders.size(); ++i) {
    const auto& order = orders[i];
    if (!Working(order)) continue;
    const auto found = rates.find(RateKey(order));
    if (found == rates.end()) return "missing_order_rates " + order.symbol;
    OrderInfo info{};
    info.side = order.direction == THOST_FTDC_D_Buy ? Side::kBuy : Side::kSell;
    const auto offset = OrderOffset(order.offset);
    if (!offset) return "unsupported_order_offset " + order.symbol;
    info.offset = *offset;
    const auto& terms = info.side == Side::kBuy ? found->second.buy : found->second.sell;
    if (!terms.hedge) return "missing_order_rates " + order.symbol;
    if (order.currency && *order.currency != terms.currency) {
      return "order_currency_mismatch " + order.symbol;
    }
    info.hedge = *terms.hedge;
    const auto cost = CalculateOrderCost(info, terms, order.price, order.remaining);
    if (!cost) return "invalid_order_cost " + order.symbol;
    auto total = funds.find(terms.currency.code());
    if (total == funds.end()) return "missing_hold_currency " + order.symbol;
    total->second.margin += cost->margin;
    total->second.commission += cost->fee;
    holds.push_back({i, order.remaining, terms.currency, cost->margin, cost->fee, 0, 0, terms});
    if (info.offset != Offset::kOpen) AddClose(order, holds, positions);
  }
  for (const auto& b : snapshot.balances) {
    const auto& sum = funds.at(b.currency.code());
    const std::string currency(b.currency.code());
    if (b.frozen_cash != 0) return "unattributed_frozen_cash " + currency;
    if (!SameMoney(sum.margin, b.frozen_margin)) return "frozen_margin_mismatch " + currency;
    if (!SameMoney(sum.commission, b.frozen_commission)) {
      return "frozen_commission_mismatch " + currency;
    }
  }
  return AssignBuckets(positions, holds);
}

}  // namespace

std::string CollectOrderRates(const SimNowOrderHistory& history, OrderRates& rates) {
  for (const auto& o : history.orders()) {
    if (!Working(o)) {
      switch (o.status) {
        case THOST_FTDC_OST_AllTraded:
        case THOST_FTDC_OST_Canceled:
        case THOST_FTDC_OST_PartTradedNotQueueing:
        case THOST_FTDC_OST_NoTradeNotQueueing: continue;
        default: return "unresolved_order_status " + o.symbol;
      }
    }
    if (o.remaining <= 0 || o.remaining != o.quantity - o.traded ||
        (o.status == THOST_FTDC_OST_NoTradeQueueing) != (o.traded == 0) ||
        o.submit_status == THOST_FTDC_OSS_InsertRejected) {
      return "inconsistent_working_order " + o.symbol;
    }
    if (o.exchange.empty() || o.multi_leg || o.price_type != THOST_FTDC_OPT_LimitPrice ||
        !OrderOffset(o.offset) || (o.hedge != THOST_FTDC_HF_Speculation &&
        o.hedge != THOST_FTDC_HF_Hedge && o.hedge != THOST_FTDC_HF_Arbitrage)) {
      return "unsupported_order_hold " + o.symbol;
    }
    if (o.symbol.size() >= sizeof(TThostFtdcInstrumentIDType) ||
        o.exchange.size() >= sizeof(TThostFtdcExchangeIDType) ||
        o.invest_unit.size() >= sizeof(TThostFtdcInvestUnitIDType)) {
      return "invalid_rate_scope " + o.symbol;
    }
    rates.try_emplace(RateKey(o));
  }
  return {};
}

void ReconcileOrderHolds(SimNowStartupSnapshot& snapshot, const OrderRates& rates) {
  std::vector<SimNowOrderHold> holds;
  snapshot.holds_error = Associate(snapshot, rates, holds);
  if (snapshot.holds_error.empty()) snapshot.holds = std::move(holds);
  else snapshot.holds.clear();
}

}  // namespace yquant::trading::internal
