#include "yquant/trading/trading_service.h"

#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <utility>
#include <variant>

#include "yquant/trading/position.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {
namespace {

// Validates the identity used by incoming rows and individual queries.
bool ValidKey(const PositionKey& key) {
  return key.instrument_id != 0 &&
         (key.side == PositionSide::kLong ||
          key.side == PositionSide::kShort) &&
         (key.hedge == HedgeFlag::kSpeculation ||
          key.hedge == HedgeFlag::kArbitrage || key.hedge == HedgeFlag::kHedge);
}

// Checks each quantity before saving it in the pending account snapshot.
bool ValidPosition(const PositionInfo& value) {
  return ValidKey(value.key) && value.today_quantity >= 0 &&
         value.yesterday_quantity >= 0 && value.today_frozen >= 0 &&
         value.today_frozen <= value.today_quantity &&
         value.yesterday_frozen >= 0 &&
         value.yesterday_frozen <= value.yesterday_quantity;
}

}  // namespace

bool TradingService::QueryPositions(const QueryPositionsRequest& query,
                                    PositionsSnapshot& snapshot) const {
  const auto found = account_index_.find(query.account_id);
  if (found == account_index_.end() || !found->second->positions_available) {
    return false;
  }
  const auto& account = *found->second;
  snapshot = {account.positions.rows, account.positions_snapshot_id};
  return true;
}

bool TradingService::QueryPosition(const QueryPositionRequest& query,
                                   PositionInfo& position) const {
  const auto found = account_index_.find(query.account_id);
  if (found == account_index_.end() || !found->second->positions_available ||
      !ValidKey(query.key)) {
    return false;
  }
  const auto& book = found->second->positions;
  const auto row = book.index.find(query.key);
  position = row == book.index.end() ? PositionInfo{query.key}
                                   : book.rows[row->second];
  return true;
}

void TradingService::AccountContext::OnPositionsReport(
    const AdapterPositionsReport& report) {
  owner.ApplyPositions(*this, report);
}

void TradingService::FailPositions(AccountContext& account,
                                   const char* reason) {
  account.positions_refresh_pending = false;
  account.positions_available = false;
  std::fprintf(stderr, "trading %s account=%" PRIu64 " refresh=%" PRIu64 "\n",
               reason, account.id, account.positions_refresh_id);
}

void TradingService::StagePosition(AccountContext& account,
                                   const PositionInfo& position) {
  if (!ValidPosition(position)) {
    FailPositions(account, "invalid_position");
    return;
  }
  auto& book = account.positions_staging;
  const auto found = book.index.find(position.key);
  if (found != book.index.end()) {
    if (book.rows[found->second] != position) {
      FailPositions(account, "conflicting_position");
    }
    return;
  }
  book.index.emplace(position.key, book.rows.size());
  book.rows.push_back(position);
}

void TradingService::CompletePositions(AccountContext& account,
                                       std::size_t expected_count) {
  if (account.positions_staging.rows.size() != expected_count) {
    FailPositions(account, "incomplete_positions");
    return;
  }
  std::swap(account.positions, account.positions_staging);
  account.positions_snapshot_id = account.positions_refresh_id;
  account.positions_refresh_pending = false;
  account.positions_available = true;
}

void TradingService::ApplyPositions(AccountContext& account,
                                    const AdapterPositionsReport& report) {
  if (report.refresh_id == 0) {
    std::fprintf(stderr, "trading invalid_positions_refresh account=%" PRIu64
                         " refresh=0\n", account.id);
    return;
  }
  if (std::holds_alternative<PositionsRefreshStarted>(report.event)) {
    if (report.refresh_id > account.positions_refresh_id) {
      account.positions_refresh_id = report.refresh_id;
      account.positions_refresh_pending = true;
      account.positions_staging.rows.clear();
      account.positions_staging.index.clear();
    }
    return;
  }
  if (report.refresh_id != account.positions_refresh_id ||
      !account.positions_refresh_pending) {
    return;
  }
  if (const auto* row = std::get_if<PositionInfo>(&report.event)) {
    StagePosition(account, *row);
  } else if (const auto* end =
                 std::get_if<PositionsRefreshCompleted>(&report.event)) {
    CompletePositions(account, end->expected_count);
  } else {
    FailPositions(account, "positions_refresh_failed");
  }
}

}  // namespace yquant::trading
