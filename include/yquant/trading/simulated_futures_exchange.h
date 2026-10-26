#ifndef YQUANT_TRADING_SIMULATED_FUTURES_EXCHANGE_H_
#define YQUANT_TRADING_SIMULATED_FUTURES_EXCHANGE_H_

#include <stdint.h>

#include <memory>
#include <optional>

#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Single-thread, quote-driven limit exchange. Each quote supplies finite bid
// and ask liquidity shared FIFO by orders. Prepare owns all bounded storage;
// sends/quotes/Poll allocate nothing and never invoke strategy callbacks.
class SimulatedFuturesExchange final : public TradingAdapter {
 public:
  SimulatedFuturesExchange() noexcept;
  ~SimulatedFuturesExchange() noexcept override;
  SimulatedFuturesExchange(const SimulatedFuturesExchange&) = delete;
  SimulatedFuturesExchange& operator=(const SimulatedFuturesExchange&) = delete;

  // Validates/copies configuration and allocates once. Failure retains the
  // unprepared state and remains safe to Stop; no account/output is published.
  [[nodiscard]] base::Status Prepare(
      const TradingConfig& config) noexcept override;
  // Activates a prepared empty session without creating workers or callbacks.
  [[nodiscard]] base::Status Start(uint64_t now) noexcept override;
  // Available only after Start and before any order; failure preserves out.
  [[nodiscard]] base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override;
  // Copies a unique positive limit request and plans its asynchronous Ack plus
  // any executable quantity. Failure preserves all orders, plans and liquidity.
  [[nodiscard]] base::Status SendOrder(model::ClientOrderId id,
                                       const model::OrderRequest& request,
                                       uint64_t now) noexcept override;
  // Requires an acknowledged identity and a new cancel attempt. Also settles
  // already-filled orders with a rejection. Failure queues nothing and keeps
  // the previous cancel state; request inputs are borrowed for this call only.
  [[nodiscard]] base::Status SendCancel(
      model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
      model::CancelRequestId cancel_request_id, uint64_t now) noexcept override;

  // Copies an accepted quote and matches existing orders at their limits. New
  // aggressive orders instead trade at the executable opposing quote. An old
  // sequence is ignored. Failure preserves the book and matching facts.
  // Cancels take effect at SendCancel time plus report_interval; quotes at or
  // after that boundary cannot match the order, even before its result is read.
  [[nodiscard]] base::Status OnQuote(const model::QuoteTick& quote,
                                     uint64_t now) noexcept override;
  // Publishes one normalized event in deadline/FIFO order. Ack is delayed by
  // report_interval. Quote-triggered fills are immediately pollable after Ack;
  // they remain ordered before a later cancel result. Idle/error preserve out.
  [[nodiscard]] TradingPollResult Poll(
      uint64_t now, model::TradingEvent& out) noexcept override;
  std::optional<uint64_t> next_deadline() const noexcept override;
  bool is_drained() const noexcept override;
  [[nodiscard]] base::Status Stop() noexcept override;
  // Stops the session and discards its state and diagnostics. Allows Prepare
  // to begin a new session; all operations require serial access.
  void Reset() noexcept;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  struct Storage;
  enum class State { kUnprepared, kPrepared, kActive, kFailed, kStopped };
  // Copies a diagnostic without changing orders, queued events or caller
  // output.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Makes polling terminal; retained facts remain available until Stop.
  TradingPollResult PollError(base::ErrorCode code,
                              const char* detail) noexcept;

  std::unique_ptr<Storage> storage_;
  State state_ = State::kUnprepared;
  bool drained_on_stop_ = true;
  char error_detail_[768] = {};
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_SIMULATED_FUTURES_EXCHANGE_H_
