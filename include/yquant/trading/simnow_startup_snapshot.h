#ifndef YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_
#define YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "yquant/model/currency.h"
#include "yquant/trading/account_recovery.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/position.h"
#include "yquant/trading/simnow_order_history.h"

namespace yquant::trading {

// One currency balance as returned by the counter. Available already excludes
// the counter's holds. Amounts have not been converted to the local book basis.
struct SimNowBalance {
  model::Currency currency;
  double balance = 0;
  double available = 0;
  double position_profit = 0;
  double margin = 0;
  double frozen_margin = 0;
  double frozen_cash = 0;
  double frozen_commission = 0;
};

// One counter row with verified today and prior-day costs. Date, direction
// and hedge retain their CTP codes; position_cost and margin are row totals.
struct SimNowPosition {
  std::string exchange, symbol, invest_unit;
  char direction = 0, hedge = 0, date = 0;
  int quantity = 0;
  int today = 0;
  // YdPosition is the counter's prior-day quantity, before today's trades.
  int previous_day_quantity = 0;
  int long_frozen = 0, short_frozen = 0;
  double position_cost = 0, open_cost = 0;
  double margin = 0, previous_settlement = 0;
  // Each bucket holds total cost and occupied margin in account currency.
  // Empty buckets have zero amounts.
  PositionCost today_cost, yesterday_cost;
  int yesterday_quantity() const { return quantity - today; }
};

// Holds the two opening margin schedules and offset-specific fees for one
// futures contract and the hedge category used for the query.
struct SimNowContractTerms {
  std::string symbol;
  FundsTerms buy, sell;
};

// Links the unfilled part of a working order to its existing counter holds.
// order_index addresses history.orders() in this snapshot, even without an owner.
struct SimNowOrderHold {
  std::size_t order_index;
  int remaining;
  model::Currency currency;
  double margin = 0, commission = 0;
  int today = 0, yesterday = 0;
  FundsTerms terms;
};

// Collects startup query results for the logged-in account and trading day.
// Funds and positions were sampled by separate requests.
struct SimNowStartupSnapshot {
  std::string trading_day;
  std::vector<SimNowBalance> balances;
  std::vector<SimNowPosition> positions;
  SimNowOrderHistory history;
  std::optional<SimNowContractTerms> terms;
  // On success contains all working orders (including orders with zero cost).
  // On failure holds stays empty and holds_error identifies the discrepancy.
  std::vector<SimNowOrderHold> holds;
  std::string holds_error;
};

// A generic close spanning both dates needs the counter's actual fill priority.
// Single-bucket holds can be booked without configuring a priority.
enum class SimNowClosePriority { kUnspecified, kYesterdayFirst, kTodayFirst };
// Maps a counter contract to an internal instrument and its close priority.
struct SimNowInstrumentBinding {
  std::uint32_t instrument_id;
  SimNowClosePriority close_priority = SimNowClosePriority::kUnspecified;
};
using SimNowInstruments =
    std::map<std::pair<std::string, std::string>, SimNowInstrumentBinding>;

// Converts a verified snapshot to the trading book's accounting basis.
// Returns an empty string on success, or a diagnostic without changing output
// on failure. Account synchronization must still confirm that historical and
// live events form a consistent sequence before publishing the baseline.
std::string BuildAccountRecovery(const SimNowStartupSnapshot& snapshot,
                                 const SimNowInstruments& instruments,
                                 AccountRecovery& output);

enum class SnapshotPhase { kIdle, kFunds, kPositions, kPositionMultiplier, kPositionDetails, kOrders, kTrades, kContractTerms, kComplete, kFailed };

// A failed query identifies the operation or validation error and SDK code.
struct SnapshotStatus {
  SnapshotPhase phase = SnapshotPhase::kIdle;
  const char* reason = "";
  int code = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_
