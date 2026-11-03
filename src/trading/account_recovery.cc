#include "yquant/trading/trading_service.h"

#include <cmath>
#include <limits>

namespace yquant::trading {

void TradingService::AccountContext::OnAccountRecovery(
    std::uint64_t attempt, AccountRecovery baseline) {
  if (sync_state != SyncState::kPending || attempt != sync_id) return;
  if (recovery || funds_available || positions_available ||
      funds_refresh_pending || positions_refresh_pending) {
    owner.FailAccountSync(*this, "conflicting_recovery_baseline");
    return;
  }
  recovery = std::move(baseline);
}

void TradingService::AccountContext::OnRecoveredEvent(const RecoveredEvent& event) {
  if (sync_state == SyncState::kPending && recovery) {
    owner.FailAccountSync(*this, "unconfirmed_recovery_event");
    return;
  }
  if (sync_state != SyncState::kReady) return;
  if (std::holds_alternative<RecoveryRoutingFailure>(event)) {
    funds_available = false;
    owner.Diagnose("unresolved_recovered_report", id, {});
    return;
  }
  const auto* fill = std::get_if<RecoveredFill>(&event);
  const auto* cumulative = std::get_if<RecoveredCumulative>(&event);
  const auto key = fill ? fill->key : cumulative ? cumulative->key :
      std::get<RecoveredCancellation>(event).key;
  const auto found = recovered_orders.find(key);
  if (found == recovered_orders.end()) {
    funds_available = false;
    owner.Diagnose("unknown_recovered_order", id, {});
    return;
  }
  auto& order = *found->second;
  if (fill) {
    owner.ApplyTrade(*this, order, {order.order.order_id, fill->trade_id,
                                    fill->price, fill->quantity});
  } else if (cumulative) {
    const auto filled = cumulative->filled_quantity;
    if (filled < 0 || filled > order.order.quantity) {
      funds_available = false;
      owner.Diagnose("invalid_recovered_fill_quantity", id, order.order.order_id);
    } else if (filled > order.reported_filled_quantity) {
      const bool changed = UpdateFill(order, order.state, filled, order.detailed_filled_quantity);
      owner.UpdateReservation(*this, order);
      if (changed) owner.Notify({order.listener, OrderReport{MakeSnapshot(order)}, &order, order.call});
    }
  } else {
    const int filled = std::get<RecoveredCancellation>(event).filled_quantity;
    if (owner.ConfirmCancellation(*this, order, filled)) {
      owner.Notify({order.listener, OrderReport{MakeSnapshot(order)}, &order, order.call});
    }
  }
}

// Stage all indexes together. Pointers into owned map nodes stay valid across
// merge, and anonymous orders never enter the caller's OrderId namespace.
const char* TradingService::InstallRecovery(AccountContext& account,
                                           const AccountRecovery& baseline) {
  const auto& funds = baseline.funds;
  if (!funds.currency.valid() || !std::isfinite(funds.balance) ||
      !std::isfinite(funds.available) || !std::isfinite(funds.margin) ||
      funds.margin < 0 || !std::isfinite(funds.frozen_margin) ||
      funds.frozen_margin < 0) return "invalid_recovery_funds";
  PositionsBook book;
  book.rows = baseline.positions;
  for (std::size_t i = 0; i < book.rows.size(); ++i) {
    if (!ValidPosition(book.rows[i]) ||
        !book.index.emplace(book.rows[i].key, i).second) return "invalid_recovery_position";
  }
  decltype(orders_) owned;
  decltype(account.unowned_orders) unowned;
  decltype(account.recovered_orders) native;
  decltype(account.trades) trades;
  std::vector<std::pair<int,int>> frozen(book.rows.size());
  long double reserved = 0, margin = 0;
  std::size_t count = 0;
  for (const auto& o : baseline.orders) {
    OrderInfo info{o.owner ? o.owner->order_id : OrderId{}, account.id,
        o.owner ? o.owner->strategy_id : 0, o.instrument, o.side, o.offset,
        o.price, o.quantity, o.hedge};
    if ((o.owner && !info.order_id.sequence) || !o.instrument ||
        (o.side != Side::kBuy && o.side != Side::kSell) ||
        (o.hedge != HedgeFlag::kSpeculation && o.hedge != HedgeFlag::kArbitrage &&
         o.hedge != HedgeFlag::kHedge) ||
        static_cast<unsigned>(o.offset) >= o.terms.fees.size() ||
        o.booked < 0 || o.booked >= o.quantity || !ValidTerms(o.terms) ||
        o.terms.currency != funds.currency) return "invalid_recovered_order";
    const int left = o.quantity - o.booked;
    const auto cost = internal::CalculateOrderCost(info, o.terms, o.price, left);
    if (!cost || cost->margin != o.margin || cost->fee != o.commission) {
      return "recovery_order_cost_mismatch";
    }
    OrderRecord record{info, nullptr, {}};
    record.state = o.booked ? OrderState::kPartiallyFilled : OrderState::kAccepted;
    record.baseline_booked = record.booked_quantity = record.reported_filled_quantity = o.booked;
    record.recovered_terms = std::make_unique<FundsTerms>(o.terms);
    record.initial_reservation = record.reserved_funds = cost->total();
    record.initial_margin = record.reserved_margin = cost->margin;
    if (o.offset == Offset::kOpen) {
      if (o.today || o.yesterday) return "invalid_open_position_hold";
    } else {
      if (o.today < 0 || o.today > left || o.yesterday != left - o.today ||
          (o.offset == Offset::kCloseToday && o.yesterday) ||
          (o.offset == Offset::kCloseYesterday && o.today)) return "invalid_close_allocation";
      const auto it = book.index.find(PositionKey::ForOrder(info));
      if (it == book.index.end()) return "missing_recovered_position";
      const auto& row = book.rows[it->second];
      auto& [today, yesterday] = frozen[it->second];
      if (o.today > row.today_frozen - today || o.yesterday > row.yesterday_frozen - yesterday) {
        return "recovery_position_hold_mismatch";
      }
      today += o.today;
      yesterday += o.yesterday;
      const bool first = o.offset == Offset::kCloseToday || o.terms.close_today_first;
      record.close = {it->second, first, first ? o.today : o.yesterday, left};
    }
    reserved += record.reserved_funds;
    margin += o.margin;
    if (record.reserved_funds > 0) ++count;
    OrderRecord* stored;
    if (o.owner) {
      if (orders_.contains(info.order_id)) return "duplicate_recovered_order";
      const auto [it, added] = owned.emplace(info.order_id, std::move(record));
      if (!added) return "duplicate_recovered_order";
      stored = &it->second;
    } else {
      stored = unowned.emplace_back(std::make_unique<OrderRecord>(std::move(record))).get();
    }
    if (!native.emplace(o.key, stored).second) return "duplicate_recovery_key";
  }
  for (const auto& f : baseline.fills) {
    const auto it = native.find(f.key);
    if (it == native.end() || f.quantity <= 0 || !std::isfinite(f.price) || f.price <= 0 ||
        f.trade_id.venue.empty() || f.trade_id.session.empty() || f.trade_id.value.empty() ||
        !trades.emplace(f.trade_id, RecordedFill{it->second, f.price, f.quantity}).second) {
      return "invalid_recovery_fill";
    }
    auto& order = *it->second;
    if (f.quantity > order.booked_quantity - order.detailed_filled_quantity) {
      return "recovery_fill_quantity_mismatch";
    }
    order.detailed_filled_quantity += f.quantity;
  }
  for (const auto& [key, order] : native) {
    if (order->detailed_filled_quantity != order->booked_quantity) {
      return "missing_recovery_fill";
    }
  }
  for (std::size_t i = 0; i < book.rows.size(); ++i) {
    if (frozen[i].first != book.rows[i].today_frozen ||
        frozen[i].second != book.rows[i].yesterday_frozen) return "unassociated_position_hold";
  }
  if (reserved > std::numeric_limits<double>::max() ||
      (margin == 0) != (funds.frozen_margin == 0) ||
      std::abs(margin - funds.frozen_margin) >= 0.005L) return "recovery_funds_mismatch";
  auto terms = account.funds_terms;
  for (const auto& [instrument, rates] : baseline.order_terms) {
    if (!instrument || !ValidTerms(rates.first) || !ValidTerms(rates.second) ||
        rates.first.currency != funds.currency || rates.second.currency != funds.currency) {
      return "invalid_startup_order_terms";
    }
    terms.insert_or_assign(TermsKey(instrument, Side::kBuy), rates.first);
    terms.insert_or_assign(TermsKey(instrument, Side::kSell), rates.second);
  }
  account.funds_terms = std::move(terms);
  account.last_sequences = baseline.last_sequences;
  orders_.merge(owned);
  account.unowned_orders = std::move(unowned);
  account.recovered_orders = std::move(native);
  account.trades = std::move(trades);
  account.positions = std::move(book);
  account.positions_snapshot_id = account.sync_id;
  account.funds = {funds, account.sync_id};
  account.reservation_count = count;
  account.reserved_funds = reserved;
  account.reserved_margin = margin;
  account.positions_available = account.funds_available = true;
  return nullptr;
}

}  // namespace yquant::trading
