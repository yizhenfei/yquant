#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cstdint>

#include "yquant/trading/internal/order_state.h"
#include "yquant/trading/order.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

void TradingService::AccountContext::OnTradeReport(const TradeInfo& trade) {
  owner.ApplyTrade(*this, trade);
}

void TradingService::ApplyTrade(AccountContext& account,
                                const TradeInfo& trade) {
  auto* order = FindReportedOrder(account.id, trade.order_id);
  if (order == nullptr) {
    return;
  }
  if (!internal::ValidTrade(trade) || order->state == OrderState::kRejected) {
    Diagnose("invalid_trade", account.id, trade.order_id);
    return;
  }
  const auto found = account.trades.find(trade.trade_id);
  if (found != account.trades.end()) {
    if (found->second != trade) {
      Diagnose("conflicting_trade", account.id, trade.order_id);
    }
    return;
  }
  // Subtract before addition so an excessive quantity cannot overflow.
  const auto remaining =
      order->order.quantity - order->detailed_filled_quantity;
  if (trade.quantity > remaining) {
    Diagnose("trade_overfill", account.id, trade.order_id);
    return;
  }
  if (order->state == OrderState::kSendFailed) {
    Diagnose("report_after_send_failure", account.id, trade.order_id);
  }
  account.trades.emplace(trade.trade_id, trade);
  UpdateFill(*order, order->state, order->reported_filled_quantity,
             order->detailed_filled_quantity + trade.quantity);
  Notify({order->listener, TradeReport{trade, MakeSnapshot(*order)}, order,
          order->call});
}

bool TradingService::UpdateFill(OrderRecord& order, OrderState state,
                                std::int32_t reported, std::int32_t detailed) {
  const auto previous = std::max(order.reported_filled_quantity,
                                order.detailed_filled_quantity);
  const auto filled = std::max(reported, detailed);
  const auto next =
      internal::StateAfterFill(state, filled, order.order.quantity);
  order.reported_filled_quantity = reported;
  order.detailed_filled_quantity = detailed;
  if (order.state == next && previous == filled) {
    return false;
  }
  order.state = next;
  ++order.version;
  return true;
}

TradingService::OrderRecord* TradingService::FindReportedOrder(
    std::uint64_t account_id, OrderId id) {
  const auto found = orders_.find(id);
  if (found == orders_.end() || found->second.order.account_id != account_id) {
    Diagnose("invalid_report", account_id, id);
    return nullptr;
  }
  return &found->second;
}

}  // namespace yquant::trading
