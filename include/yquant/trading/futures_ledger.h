#ifndef YQUANT_TRADING_FUTURES_LEDGER_H_
#define YQUANT_TRADING_FUTURES_LEDGER_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "yquant/base/status.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/model/futures.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_account.h"

namespace yquant::trading {

// Per-order remaining resource claim, owned by the order ledger. Mutate only
// through FuturesLedger; definitive cancellation releases it, uncertain sends
// retain it. Closing quantities freeze the selected direction/day bucket.
struct FuturesReservation {
  int64_t quantity = 0;
  int64_t cash = 0;
};

// Single-thread simulated futures account with bounded FIFO opening fills.
// Configure allocates once; all later operations are allocation-free atomic
// transactions. It models gross margin and configured fees, not CTP settlement.
class FuturesLedger {
 public:
  FuturesLedger() noexcept;
  ~FuturesLedger() noexcept;
  FuturesLedger(const FuturesLedger&) = delete;
  FuturesLedger& operator=(const FuturesLedger&) = delete;

  // Borrows the non-null immutable dictionary until Reset. Invalid metadata,
  // duplicate positions, arithmetic overflow or allocation failure leaves it
  // disabled.
  [[nodiscard]] base::ErrorCode Configure(
      const TradingAccountSnapshot& account,
      const md::InstrumentScope* market_data) noexcept;
  void Reset() noexcept;

  // Open requires sufficient available margin plus fees. A valid close may
  // reduce exposure even with negative available; its fee remains reserved.
  // Failure preserves the account, position and out.
  [[nodiscard]] base::ErrorCode Reserve(const model::OrderRequest& request,
                                        FuturesReservation& out) noexcept;
  // Definitively releases the remaining order claim. Failure preserves both
  // ledgers and reservation; success sets reservation to zero.
  [[nodiscard]] base::ErrorCode Release(
      const model::OrderSnapshot& order,
      FuturesReservation& reservation) noexcept;
  // Applies one protocol-validated unique execution and advances the exact
  // remaining reservation. All checks precede any account or FIFO mutation.
  // Confirmed fills do not fail merely because equity/available became
  // negative.
  [[nodiscard]] base::ErrorCode ApplyFill(
      const model::OrderSnapshot& order, const model::Fill& fill,
      FuturesReservation& reservation) noexcept;
  // Conservatively marks longs at bid, shorts at ask, gross margin at max.
  // Incomplete/nonpositive quotes preserve the previous mark and succeed.
  // Unknown/non-futures instruments are rejected; failure preserves all state.
  [[nodiscard]] base::ErrorCode ApplyQuote(
      const model::QuoteTick& tick) noexcept;
  [[nodiscard]] bool TryGetAccount(
      model::FuturesAccountSnapshot& out) const noexcept;
  [[nodiscard]] bool TryGetPosition(
      model::InstrumentId id,
      model::FuturesPositionSnapshot& out) const noexcept;
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  struct Storage;
  // Reads exact FIFO cost without consuming lots; failure preserves out.
  base::ErrorCode ReadOpeningCost(size_t position_index, size_t bucket_index,
                                  int64_t quantity, int64_t& out) noexcept;
  // Copies diagnostics while preserving all account and reservation facts.
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  std::unique_ptr<Storage> storage_;
  const md::InstrumentScope* instruments_ = nullptr;
  char error_detail_[256] = {};
};
}  // namespace yquant::trading

#endif  // YQUANT_TRADING_FUTURES_LEDGER_H_
