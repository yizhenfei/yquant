#ifndef YQUANT_TRADING_ACCOUNT_RECOVERY_H_
#define YQUANT_TRADING_ACCOUNT_RECOVERY_H_

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "yquant/trading/funds.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

// Identifies the YQuant caller and strategy of a recovered order.
struct RecoveredOwner {
  OrderId order_id;
  std::uint64_t strategy_id = 0;
};

// Describes the remaining hold for a working order. key identifies the native
// order within this account's baseline, independently of strategy ownership.
struct RecoveredOrder {
  std::size_t key;
  // Absent when the order cannot be attributed to a YQuant caller and strategy.
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
// Counter cumulative quantity can lead the corresponding priced executions.
struct RecoveredCumulative {
  std::size_t key;
  int filled_quantity;
};
// An account event could not be matched consistently to its native order.
struct RecoveryRoutingFailure {};
using RecoveredEvent = std::variant<RecoveredFill, RecoveredCancellation, RecoveredCumulative, RecoveryRoutingFailure>;

// Provides the initial account state used to resume local accounting.
// Funds follow FundsInfo conventions; positions include existing freezes, and
// fills contain the already-booked executions of recovered working orders.
// Publish only after the matching account-sync completion report.
struct AccountRecovery {
  FundsInfo funds;
  std::vector<PositionInfo> positions;
  std::vector<RecoveredOrder> orders;
  std::vector<RecoveredFill> fills;
  // Buy and sell terms, in that order, for new speculative orders by instrument.
  std::unordered_map<std::uint32_t, std::pair<FundsTerms, FundsTerms>> order_terms;
  // Highest observed sequence per caller, including terminal counter orders.
  std::unordered_map<std::uint64_t, std::uint64_t> last_sequences;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_ACCOUNT_RECOVERY_H_
