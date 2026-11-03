#ifndef YQUANT_TRADING_ACCOUNT_RECOVERY_H_
#define YQUANT_TRADING_ACCOUNT_RECOVERY_H_

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include "yquant/trading/funds.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

// The original caller identity is absent for an external working order.
struct RecoveredOwner {
  OrderId order_id;
  std::uint64_t strategy_id = 0;
};

// Describes the remaining hold for a working order. key identifies the native
// order within this account's baseline, independently of strategy ownership.
struct RecoveredOrder {
  std::size_t key;
  std::optional<RecoveredOwner> owner;
  std::uint32_t instrument;
  Side side;
  Offset offset;
  HedgeFlag hedge;
  double price;
  int quantity, booked;
  double margin, commission;
  int today, yesterday;
  FundsTerms terms;
};

// One priced execution addressed by the adapter's account-local order key.
struct RecoveredFill {
  std::size_t key;
  TradeId trade_id;
  double price;
  int quantity;
};

// Final counter confirmation, including executions whose detail may arrive later.
struct RecoveredCancellation {
  std::size_t key;
  int filled_quantity;
};
// An account event could not be matched consistently to its native order.
struct RecoveryRoutingFailure {};
using RecoveredEvent = std::variant<RecoveredFill, RecoveredCancellation, RecoveryRoutingFailure>;

// Owns a single-currency accounting baseline. Funds use local book units,
// positions already include all freezes and fills include the booked history
// of working orders. A separate sync completion certifies the live boundary.
struct AccountRecovery {
  FundsInfo funds;
  std::vector<PositionInfo> positions;
  std::vector<RecoveredOrder> orders;
  std::vector<RecoveredFill> fills;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_ACCOUNT_RECOVERY_H_
