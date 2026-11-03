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

// Verifies position costs for a single startup attempt. Prepare takes rows
// sorted by exchange and symbol. Their storage must remain stable until all
// detail queries finish or the attempt is discarded.
class SimNowPositionCosts {
 public:
  const char* Prepare(std::span<SimNowPosition> rows);
  bool complete() const { return groups_.empty(); }
  const SimNowPosition& contract() const { return groups_.back().front(); }
  // Accepts one multiplier for the active futures contract.
  bool SetMultiplier(const CThostFtdcInstrumentField& row);
  bool has_multiplier() const { return multiplier_ != 0; }
  // Returns a diagnostic for invalid rows or totals, nullptr on success.
  const char* Add(const CThostFtdcInvestorPositionDetailField& row,
                  std::string_view trading_day);
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
