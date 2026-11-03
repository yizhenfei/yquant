#ifndef YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_
#define YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_

#include <optional>
#include <string>
#include <vector>

#include "yquant/model/currency.h"
#include "yquant/trading/funds_terms.h"

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

// One counter row. Date, direction and hedge retain their CTP codes. On
// exchanges that combine dates, cost and margin cover the entire row.
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
};

// Holds the two opening margin schedules and offset-specific fees for one
// futures contract and the hedge category used for the query.
struct SimNowContractTerms {
  std::string symbol;
  FundsTerms buy, sell;
};

// Owns completed query results for the logged-in account and trading day.
// Funds and positions were sampled by separate requests.
struct SimNowStartupSnapshot {
  std::string trading_day;
  std::vector<SimNowBalance> balances;
  std::vector<SimNowPosition> positions;
  std::optional<SimNowContractTerms> terms;
};

enum class SnapshotPhase { kIdle, kFunds, kPositions, kContractTerms, kComplete, kFailed };

// A failed query identifies the operation or validation error and SDK code.
struct SnapshotStatus {
  SnapshotPhase phase = SnapshotPhase::kIdle;
  const char* reason = "";
  int code = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMNOW_STARTUP_SNAPSHOT_H_
