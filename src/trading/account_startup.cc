#include "yquant/trading/trading_service.h"

#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <limits>

#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

bool TradingService::IsAccountReady(std::uint64_t account_id) const {
  const auto found = account_index_.find(account_id);
  return found != account_index_.end() &&
         found->second->sync_state == SyncState::kReady;
}

bool TradingService::RetryAccountSync(std::uint64_t account_id) {
  const auto found = account_index_.find(account_id);
  if (running_ || request_depth_ || callback_depth_ ||
      found == account_index_.end() ||
      found->second->sync_state != SyncState::kFailed ||
      found->second->sync_id == std::numeric_limits<std::uint64_t>::max()) {
    return false;
  }
  found->second->sync_state = SyncState::kNotStarted;
  return true;
}

void TradingService::StartAccountSync(AccountContext& account) {
  ++account.sync_id;
  account.sync_state = SyncState::kPending;
  account.funds_available = false;
  account.positions_available = false;
  account.funds_refresh_pending = false;
  account.positions_refresh_pending = false;
  account.positions_staging.rows.clear();
  account.positions_staging.index.clear();
  if (!account.adapter.StartAccountSync(account.sync_id)) {
    FailAccountSync(account, "initial_sync_not_sent");
  }
}

void TradingService::FailAccountSync(AccountContext& account,
                                     const char* reason) {
  account.sync_state = SyncState::kFailed;
  account.funds_available = false;
  account.positions_available = false;
  std::fprintf(stderr, "trading %s account=%" PRIu64 " sync=%" PRIu64 "\n",
               reason, account.id, account.sync_id);
}

void TradingService::AccountContext::OnAccountSyncReport(
    const AdapterAccountSyncReport& report) {
  if (sync_state != SyncState::kPending || report.sync_id != sync_id) {
    return;
  }
  if (report.result != AccountSyncResult::kComplete) {
    owner.FailAccountSync(*this,
        report.result == AccountSyncResult::kUnresolvedOrders
            ? "initial_orders_unresolved" : "initial_sync_failed");
    return;
  }
  if (!funds_available || !positions_available) {
    owner.FailAccountSync(*this, "initial_sync_incomplete");
    return;
  }
  bool frozen = funds.funds.frozen_margin != 0;
  for (const auto& position : positions.rows) {
    frozen |= position.today_frozen != 0 || position.yesterday_frozen != 0;
  }
  if (frozen) {
    owner.FailAccountSync(*this, "initial_orders_unresolved");
    return;
  }
  sync_state = SyncState::kReady;
}

}  // namespace yquant::trading
