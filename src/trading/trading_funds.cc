#include "yquant/trading/trading_service.h"

#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <variant>

#include "yquant/trading/funds.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {
namespace {

// Checks a complete value before it replaces the published account snapshot.
bool ValidFunds(const FundsInfo& funds) {
  return funds.currency.valid() &&
         std::isfinite(funds.balance) && std::isfinite(funds.available) &&
         std::isfinite(funds.margin) && std::isfinite(funds.frozen_margin);
}

// Identifies an account refresh failure without inventing an order association.
void DiagnoseFunds(const char* reason, std::uint64_t account_id,
                   std::uint64_t refresh_id) {
  std::fprintf(stderr, "trading %s account=%" PRIu64 " refresh=%" PRIu64 "\n",
               reason, account_id, refresh_id);
}

}  // namespace

bool TradingService::QueryFunds(const QueryFundsRequest& query,
                                FundsSnapshot& snapshot) const {
  const auto found = account_index_.find(query.account_id);
  if (found == account_index_.end() ||
      found->second->sync_state != SyncState::kReady) {
    return false;
  }
  snapshot = found->second->funds;
  snapshot.reserved = static_cast<double>(found->second->reserved_funds);
  if (snapshot.reserved < found->second->reserved_funds) {
    snapshot.reserved = std::nextafter(snapshot.reserved,
                                       std::numeric_limits<double>::infinity());
  }
  return true;
}

void TradingService::AccountContext::OnFundsReport(
    const AdapterFundsReport& report) {
  owner.ApplyFunds(*this, report);
}

void TradingService::ApplyFunds(AccountContext& account,
                                const AdapterFundsReport& report) {
  if (report.refresh_id == 0) {
    DiagnoseFunds("invalid_funds_refresh", account.id, report.refresh_id);
    return;
  }
  if (account.sync_state != SyncState::kPending ||
      report.refresh_id != account.sync_id) {
    return;
  }
  if (std::holds_alternative<FundsRefreshStarted>(report.event)) {
    if (report.refresh_id > account.funds_refresh_id) {
      account.funds_refresh_id = report.refresh_id;
      account.funds_refresh_pending = true;
    }
    return;
  }
  if (report.refresh_id != account.funds_refresh_id ||
      !account.funds_refresh_pending) {
    return;
  }
  account.funds_refresh_pending = false;
  const auto* funds = std::get_if<FundsInfo>(&report.event);
  if (funds == nullptr || !ValidFunds(*funds)) {
    FailAccountSync(account, funds == nullptr ? "funds_refresh_failed"
                                             : "invalid_funds");
    return;
  }
  account.funds = {*funds, report.refresh_id};
  account.funds_available = true;
}

}  // namespace yquant::trading
