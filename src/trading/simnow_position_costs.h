#ifndef YQUANT_SRC_TRADING_SIMNOW_POSITION_COSTS_H_
#define YQUANT_SRC_TRADING_SIMNOW_POSITION_COSTS_H_

#include <ThostFtdcTraderApi.h>

#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "yquant/trading/simnow_startup_snapshot.h"

namespace yquant::trading::internal {

// Verifies today/yesterday position costs against counter totals at startup.
// Prepare requires rows sorted by exchange and symbol. This object writes costs
// into those rows; do not resize or replace their storage while queries run.
class SimNowPositionCosts {
 public:
  // Checks initial rows and identifies contracts needing detail queries.
  // Returns nullptr on success or a diagnostic on invalid data.
  // Discard the verifier and partial results on error.
  const char* Prepare(std::span<SimNowPosition> rows);
  bool complete() const { return groups_.empty(); }
  // Identifies the contract needing detail queries. Requires !complete().
  const SimNowPosition& contract() const { return groups_.back().front(); }
  // Accepts one multiplier for the active futures contract.
  bool SetMultiplier(const CThostFtdcInstrumentField& row);
  bool has_multiplier() const { return multiplier_ != 0; }
  // Returns a diagnostic for invalid rows or totals, nullptr on success.
  const char* Add(const CThostFtdcInvestorPositionDetailField& row,
                  std::string_view trading_day);
  // Checks collected details against the contract totals and advances to the
  // next contract. Returns nullptr on success, or a diagnostic on mismatch.
  const char* FinishContract();

 private:
  // Details are summed independently for each investment unit, side, hedge
  // category and age bucket of the current exchange contract.
  using Key = std::tuple<std::string, char, char, bool>;
  struct Bucket {
    int quantity = 0;
    PositionCost amounts;
  };
  std::vector<std::span<SimNowPosition>> groups_;
  std::map<Key, Bucket> buckets_;
  std::set<std::tuple<Key, std::string, std::string>> opening_trades_;
  int multiplier_ = 0;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_POSITION_COSTS_H_
