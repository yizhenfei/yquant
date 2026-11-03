#include "simnow_position_costs.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>

#include "simnow_fields.h"

namespace yquant::trading::internal {
namespace {

bool PositiveAmount(double value) {
  return value > 0 && value < std::numeric_limits<double>::max();
}

bool ValidDate(std::string_view date) {
  if (date.size() != 8 || date.find_first_not_of("0123456789") != date.npos) {
    return false;
  }
  int value = 0;
  std::from_chars(date.data(), date.data() + date.size(), value);
  const auto day = std::chrono::year(value / 10000) / (value / 100 % 100) /
                   (value % 100);
  return value >= 10000101 && day.ok();
}

// Allow half a cent of counter rounding when comparing currency totals.
bool MatchesAmount(double calculated, double reported) {
  return std::abs(calculated - reported) < 0.005;
}

}  // namespace

const char* SimNowPositionCosts::Prepare(std::span<SimNowPosition> rows) {
  while (!rows.empty()) {
    const auto next = std::find_if(rows.begin(), rows.end(), [&](const auto& r) {
      return r.exchange != rows.front().exchange || r.symbol != rows.front().symbol;
    });
    const auto group = rows.first(next - rows.begin());
    bool mixed = false;
    for (auto& row : group) {
      if (row.date == THOST_FTDC_PSD_History && row.today != 0) {
        return "invalid_position_date";
      }
      if (row.quantity == 0) {
        if (row.position_cost != 0 || row.margin != 0) return "nonzero_empty_position";
        continue;
      }
      if (!PositiveAmount(row.position_cost)) return "invalid_position_cost";
      if (row.today == 0) {
        row.yesterday_cost = {row.position_cost, row.margin};
      } else if (row.today == row.quantity) {
        row.today_cost = {row.position_cost, row.margin};
      } else {
        mixed = true;
      }
    }
    if (mixed) groups_.push_back(group);
    rows = rows.subspan(group.size());
  }
  return nullptr;
}

bool SimNowPositionCosts::SetMultiplier(const CThostFtdcInstrumentField& row) {
  if (has_multiplier() || Field(row.ExchangeID) != contract().exchange ||
      Field(row.InstrumentID) != contract().symbol ||
      row.ProductClass != THOST_FTDC_PC_Futures || row.VolumeMultiple <= 0) return false;
  multiplier_ = row.VolumeMultiple;
  return true;
}

const char* SimNowPositionCosts::Add(const CThostFtdcInvestorPositionDetailField& r,
                                     std::string_view trading_day) {
  if (Field(r.ExchangeID) != contract().exchange ||
      Field(r.InstrumentID) != contract().symbol || r.Volume < 0 ||
      !std::isfinite(r.Margin) || r.Margin < 0 ||
      r.Margin == std::numeric_limits<double>::max()) return "invalid_position_detail";
  if (r.Volume == 0) return r.Margin == 0 ? nullptr : "nonzero_empty_detail";
  const auto date = Field(r.OpenDate);
  if (!ValidDate(date) || date > trading_day || Field(r.TradeID).empty() ||
      (r.Direction != THOST_FTDC_D_Buy && r.Direction != THOST_FTDC_D_Sell)) {
    return "invalid_position_detail";
  }
  const bool today = date == trading_day;
  const char direction = r.Direction == THOST_FTDC_D_Buy ? THOST_FTDC_PD_Long
                                                         : THOST_FTDC_PD_Short;
  Key key{Field(r.InvestUnitID), direction, r.HedgeFlag, today};
  if (!opening_trades_.emplace(key, date, Field(r.TradeID)).second) {
    return "duplicate_position_detail";
  }
  const double price = today ? r.OpenPrice : r.LastSettlementPrice;
  if (!PositiveAmount(price)) return "invalid_detail_cost";
  auto& bucket = buckets_[key];
  if (r.Volume > std::numeric_limits<int>::max() - bucket.quantity) {
    return "position_detail_quantity_overflow";
  }
  bucket.quantity += r.Volume;
  bucket.amounts.cost += price * multiplier_ * r.Volume;
  bucket.amounts.margin += r.Margin;
  return nullptr;
}

const char* SimNowPositionCosts::FinishContract() {
  for (auto& row : groups_.back()) {
    for (bool today : {true, false}) {
      const int expected = today ? row.today : row.yesterday_quantity();
      if (!expected) continue;
      auto it = buckets_.find({row.invest_unit, row.direction, row.hedge, today});
      if (it == buckets_.end() || it->second.quantity != expected) {
        return "position_detail_quantity_mismatch";
      }
      (today ? row.today_cost : row.yesterday_cost) = it->second.amounts;
      buckets_.erase(it);
    }
    if (!MatchesAmount(row.today_cost.cost + row.yesterday_cost.cost, row.position_cost)) {
      return "position_detail_cost_mismatch";
    }
    if (!MatchesAmount(row.today_cost.margin + row.yesterday_cost.margin, row.margin)) {
      return "position_detail_margin_mismatch";
    }
  }
  if (!buckets_.empty()) return "unexpected_position_detail";
  opening_trades_.clear();
  groups_.pop_back();
  multiplier_ = 0;
  return nullptr;
}

}  // namespace yquant::trading::internal
