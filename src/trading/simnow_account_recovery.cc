#include "yquant/trading/simnow_startup_snapshot.h"

#include <ThostFtdcTraderApi.h>

#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace yquant::trading {
namespace {

std::optional<HedgeFlag> Hedge(char flag) {
  switch (flag) {
    case THOST_FTDC_HF_Speculation: return HedgeFlag::kSpeculation;
    case THOST_FTDC_HF_Arbitrage: return HedgeFlag::kArbitrage;
    case THOST_FTDC_HF_Hedge: return HedgeFlag::kHedge;
    default: return std::nullopt;
  }
}

std::optional<Offset> DecodeOffset(char offset) {
  switch (offset) {
    case THOST_FTDC_OF_Open: return Offset::kOpen;
    case THOST_FTDC_OF_Close: return Offset::kClose;
    case THOST_FTDC_OF_CloseToday: return Offset::kCloseToday;
    case THOST_FTDC_OF_CloseYesterday: return Offset::kCloseYesterday;
    default: return std::nullopt;
  }
}

bool AddBucket(int lots, PositionCost cost, int& quantity,
               std::optional<PositionCost>& amounts) {
  if (lots < 0 || lots > std::numeric_limits<int>::max() - quantity) return false;
  if (!lots) return cost == PositionCost{};
  if (!amounts) amounts.emplace();
  quantity += lots;
  amounts->cost += cost.cost;
  amounts->margin += cost.margin;
  return std::isfinite(amounts->cost) && std::isfinite(amounts->margin);
}

}  // namespace

std::string BuildAccountRecovery(const SimNowStartupSnapshot& s,
                                 const SimNowInstruments& instruments,
                                 AccountRecovery& output) {
  if (!s.holds_error.empty()) return s.holds_error;
  if (s.balances.size() != 1) return "expected_single_currency_book";
  std::unordered_set<std::uint32_t> ids;
  for (const auto& [symbol, id] : instruments) {
    if (!id.instrument_id || !ids.insert(id.instrument_id).second) return "ambiguous_instrument_binding";
  }
  AccountRecovery result;
  const auto& b = s.balances.front();
  result.funds = {b.currency, b.balance - b.position_profit, b.available,
                  b.margin, b.frozen_margin};
  std::unordered_map<PositionKey, std::size_t, PositionKey::Hash> rows;
  std::unordered_map<PositionKey, std::string, PositionKey::Hash> units;
  for (const auto& p : s.positions) {
    if (!p.quantity) continue;
    const auto id = instruments.find({p.exchange, p.symbol});
    if (id == instruments.end()) return "missing_instrument_binding " + p.symbol;
    const auto hedge = Hedge(p.hedge);
    if (!hedge || (p.direction != THOST_FTDC_PD_Long &&
                   p.direction != THOST_FTDC_PD_Short)) return "unsupported_position " + p.symbol;
    PositionKey key{id->second.instrument_id, p.direction == THOST_FTDC_PD_Long ?
        PositionSide::kLong : PositionSide::kShort, *hedge};
    if (units.emplace(key, p.invest_unit).first->second != p.invest_unit) {
      return "ambiguous_position_unit " + p.symbol;
    }
    const auto [it, added] = rows.emplace(key, result.positions.size());
    if (added) result.positions.push_back(PositionInfo{key});
    auto& row = result.positions[it->second];
    if (!AddBucket(p.today, p.today_cost, row.today_quantity, row.today_cost) ||
        !AddBucket(p.yesterday_quantity(), p.yesterday_cost,
                   row.yesterday_quantity, row.yesterday_cost)) return "invalid_position_cost";
  }
  std::unordered_set<std::size_t> active;
  long double held = 0;
  for (const auto& h : s.holds) {
    if (h.order_index >= s.history.orders().size() ||
        !active.insert(h.order_index).second) return "invalid_hold_order";
    const auto& o = s.history.orders()[h.order_index];
    const auto id = instruments.find({o.exchange, o.symbol});
    if (id == instruments.end()) return "missing_instrument_binding " + o.symbol;
    const auto offset = DecodeOffset(o.offset);
    const auto hedge = Hedge(o.hedge);
    if (!offset || !hedge || (o.direction != THOST_FTDC_D_Buy &&
        o.direction != THOST_FTDC_D_Sell) || h.currency != b.currency ||
        h.remaining != o.quantity - o.traded) return "invalid_recovery_hold";
    const Side side = o.direction == THOST_FTDC_D_Buy ? Side::kBuy : Side::kSell;
    OrderInfo info{};
    info.instrument_id = id->second.instrument_id;
    info.side = side;
    info.hedge = *hedge;
    info.offset = *offset;
    const auto key = PositionKey::ForOrder(info);
    if (units.emplace(key, o.invest_unit).first->second != o.invest_unit) {
      return "ambiguous_position_unit " + o.symbol;
    }
    if (*offset != Offset::kOpen) {
      const auto row = rows.find(key);
      if (row == rows.end()) return "missing_close_position";
      auto& pos = result.positions[row->second];
      if (h.today < 0 || h.today > pos.today_available() || h.yesterday < 0 ||
          h.yesterday > pos.yesterday_available()) return "invalid_close_hold";
      pos.today_frozen += h.today;
      pos.yesterday_frozen += h.yesterday;
    }
    FundsTerms terms = h.terms;
    if (h.today && h.yesterday) {
      if (id->second.close_priority == SimNowClosePriority::kUnspecified) {
        return "missing_close_priority " + o.symbol;
      }
      terms.close_today_first = id->second.close_priority == SimNowClosePriority::kTodayFirst;
    }
    std::optional<RecoveredOwner> owner;
    if (o.owner) owner = RecoveredOwner{o.owner->order_id, o.owner->strategy_id};
    result.orders.push_back({h.order_index, owner, id->second.instrument_id, side, *offset, *hedge,
        o.price, o.quantity, o.traded, h.margin, h.commission, h.today, h.yesterday, terms});
    held += h.margin + h.commission;
  }
  result.funds.available += static_cast<double>(held);
  for (const auto& [id, f] : s.history.fills()) {
    if (active.contains(f.order_index)) {
      result.fills.push_back({f.order_index, id, f.price, f.quantity});
    }
  }
  if (!std::isfinite(result.funds.balance) || !std::isfinite(result.funds.available)) {
    return "unrepresentable_recovery_balance";
  }
  output = std::move(result);
  return {};
}

}  // namespace yquant::trading
