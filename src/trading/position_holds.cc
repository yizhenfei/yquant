#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cstdint>

#include "yquant/trading/position.h"

namespace yquant::trading {

std::int32_t TradingService::CloseHold::TodayPrefix(std::int32_t n) const {
  return today_first ? std::min(first_quantity, n)
                     : n - std::min(first_quantity, n);
}

bool TradingService::FreezeClose(AccountContext& account,
                                 const OrderInfo& order, CloseHold& hold) {
  if (order.offset == Offset::kOpen) return true;
  const auto row = account.positions.index.find(PositionKey::ForOrder(order));
  if (row == account.positions.index.end()) return false;
  auto& position = account.positions.rows[row->second];
  hold.row = row->second;
  hold.today_first = order.offset == Offset::kCloseToday ||
      (order.offset == Offset::kClose &&
       FindFundsTerms(account, order)->close_today_first);
  const auto first = hold.today_first ? position.today_available()
                                      : position.yesterday_available();
  if ((order.offset != Offset::kClose && order.quantity > first) ||
      order.quantity > position.available_quantity()) return false;
  hold.first_quantity = std::min(first, order.quantity);
  const auto today = hold.TodayPrefix(order.quantity);
  position.today_frozen += today;
  position.yesterday_frozen += order.quantity - today;
  hold.held_end = order.quantity;
  return true;
}

void TradingService::UpdateCloseHold(AccountContext& account,
                                     OrderRecord& order) {
  if (order.order.offset == Offset::kOpen || !account.funds_available) return;
  auto end = order.order.quantity;
  if (order.state == OrderState::kRejected ||
      order.state == OrderState::kSendFailed) {
    end = 0;
  } else if (order.state == OrderState::kCanceled) {
    end = std::max(order.reported_filled_quantity,
                   order.detailed_filled_quantity);
  }
  end -= order.baseline_booked;
  auto& hold = order.close;
  if (end == hold.held_end) return;
  auto& position = account.positions.rows[hold.row];
  const auto today = hold.TodayPrefix(end) - hold.TodayPrefix(hold.held_end);
  const auto yesterday = (end - hold.held_end) - today;
  if (today > position.today_available() ||
      yesterday > position.yesterday_available()) {
    account.funds_available = false;
    Diagnose("close_hold_unresolved", account.id, order.order.order_id);
    return;
  }
  position.today_frozen += today;
  position.yesterday_frozen += yesterday;
  hold.held_end = end;
}

}  // namespace yquant::trading
