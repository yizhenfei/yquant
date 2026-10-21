#ifndef YQUANT_STRATEGYD_FUTURES_LEDGER_H_
#define YQUANT_STRATEGYD_FUTURES_LEDGER_H_

#include <cstdint>

#include "strategyd/trading_account.h"
#include "yquant/base/status.h"
#include "yquant/model/futures.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"

namespace yquant::strategyd {
class MarketDataImpl;

// Per-order remaining resource claim, owned by the order ledger. Mutate only
// through FuturesLedger; definitive cancellation releases it, uncertain sends
// retain it. Closing quantities freeze the selected direction/day bucket.
struct FuturesReservation {
  std::int64_t quantity = 0;
  std::int64_t cash = 0;
};

// Single-thread simulated futures account with bounded FIFO opening fills.
// Configure allocates once; all later operations are allocation-free atomic
// transactions. It models gross margin and configured fees, not CTP settlement.
class FuturesLedger {
 public:
  FuturesLedger() noexcept = default;
  ~FuturesLedger() noexcept;
  FuturesLedger(const FuturesLedger&) = delete;
  FuturesLedger& operator=(const FuturesLedger&) = delete;

  // Borrows the immutable dictionary until Reset. Invalid metadata, duplicate
  // positions, arithmetic overflow or allocation failure leaves it disabled.
  [[nodiscard]] base::ErrorCode Configure(
      const TradingAccountSnapshot& account,
      const MarketDataImpl& market_data) noexcept;
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
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  Storage* storage_ = nullptr;
  const MarketDataImpl* market_data_ = nullptr;
  char error_detail_[256] = {};
};
}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_FUTURES_LEDGER_H_
