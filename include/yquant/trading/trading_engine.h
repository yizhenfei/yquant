#ifndef YQUANT_TRADING_TRADING_ENGINE_H_
#define YQUANT_TRADING_TRADING_ENGINE_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/order_request_executor.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_dispatch.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {

// Gateway engine owning a broker session: authoritative order/account state,
// reservations, queued requests, report commits and stopping progress. All
// calls are serialized by the host. Clock, immutable instrument scope and
// adapter outlive this object; the scope matches Init's dictionary. Init
// allocates fixed storage once, and request/event processing does not allocate.
// No operation invokes strategies.
class TradingEngine final {
 public:
  // Borrows non-null dependencies; all must outlive this engine.
  TradingEngine(const base::MonotonicClock* clock,
                const md::InstrumentScope* instruments,
                TradingAdapter* adapter) noexcept;
  ~TradingEngine() noexcept;
  TradingEngine(const TradingEngine&) = delete;
  TradingEngine& operator=(const TradingEngine&) = delete;

  // Starts the adapter and validates/copies its complete initial account.
  // An initial failure leaves requests disabled and is safe to Stop/Reset;
  // kInvalidState preserves an existing session. Reset is required to replace
  // an initialized account. Inputs are call-only.
  [[nodiscard]] base::Status Init(
      const TradingConfig& config,
      const md::MarketDataConfig& dictionary) noexcept;
  // Closes requests and releases adapter resources, retaining order/account
  // facts until Reset. Idempotent, including after partial initialization.
  [[nodiscard]] base::Status Stop() noexcept;
  // Stops the adapter, frees storage and invalidates all queries.
  void Reset() noexcept;
  void SetAcceptingRequests(bool accepting) noexcept {
    accept_requests_ = accepting && !stopping_;
  }
  // Permanently closes requests and starts a bounded cancellation scan once.
  void BeginStopping() noexcept;

  bool accepting_orders() const noexcept;
  // Broker-managed futures bypass the simulated financial ledger entirely.
  bool broker_managed() const noexcept;
  [[nodiscard]] base::ErrorCode SubmitOrder(const model::OrderRequest& request,
                                            model::ClientOrderId& out) noexcept;
  [[nodiscard]] base::ErrorCode CancelOrder(model::ClientOrderId id) noexcept;
  bool TryGetOrder(model::ClientOrderId id,
                   model::OrderSnapshot& out) const noexcept;
  bool TryGetCash(model::CashSnapshot& out) const noexcept;
  bool TryGetPosition(model::InstrumentId id,
                      model::PositionSnapshot& out) const noexcept;

  [[nodiscard]] bool TryGetFuturesAccount(
      model::FuturesAccountSnapshot& out) const noexcept;
  [[nodiscard]] bool TryGetFuturesPosition(
      model::InstrumentId id,
      model::FuturesPositionSnapshot& out) const noexcept;

  // Marks an accepted quote before its strategy callback. Disabled/cash
  // accounts need no valuation and succeed. Futures failures preserve the
  // entire account/position and copy a diagnostic; incomplete quotes retain
  // the previous mark. Never invokes callbacks or allocates.
  [[nodiscard]] base::ErrorCode ApplyQuote(
      const model::QuoteTick& tick) noexcept;

  // Requires requests closed. Marks each attempt before sending; uncertain
  // failures never requeue it. Stopping converts unsent orders to local
  // cancels.
  [[nodiscard]] base::Status ProcessRequests(uint64_t now,
                                             size_t limit = 64) noexcept;
  // Scans a bounded number of slots for cancels, retaining progress on a full
  // queue. Rejected cancels wait at least 10ms before retrying.
  [[nodiscard]] base::Status ScheduleStopping(uint64_t now,
                                              size_t max_scan = 64) noexcept;
  std::optional<uint64_t> next_deadline() const noexcept;
  bool has_ready_event(uint64_t now) const noexcept;
  bool is_drained() const noexcept;
  size_t order_count() const noexcept;
  const char* error_stage() const noexcept { return error_stage_; }
  const char* error_detail() const noexcept { return error_detail_; }
  [[nodiscard]] base::Status Poll(uint64_t now, TradingDispatch& out,
                                  bool& event) noexcept;

  // Commits a supplied normalized report, preserving state and out on failure.
  // Serial access and the runtime's monotonic clock domain are required.
  [[nodiscard]] base::ErrorCode ApplyEvent(const model::TradingEvent& event,
                                           TradingDispatch& out) noexcept;
  // Commits one queued CanceledBeforeSend or CancelNotSent result. Empty input
  // returns kInvalidState without changing out or account/order facts.
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(TradingDispatch& out) noexcept;
  bool has_local_updates() const noexcept;

  // Cold/bounded retained-slot iteration; false preserves out.
  [[nodiscard]] bool TryGetOrderAt(size_t index,
                                   model::OrderSnapshot& out) const noexcept;
  size_t fill_count() const noexcept;
  // Cold retained execution iteration; false preserves out. Access must be
  // serialized with ledger mutations, matching the order iteration contract.
  [[nodiscard]] bool TryGetFillAt(size_t index,
                                  model::Fill& out) const noexcept;

 private:
  struct Storage;
  struct Submission;
  struct ReportTransaction;
  // Validates units, account compatibility, capacity and cash arithmetic.
  base::ErrorCode ValidateSubmission(const model::OrderRequest& request,
                                     Submission& transaction) noexcept;
  // Stages cash claims or atomically reserves simulated futures resources.
  base::ErrorCode ReserveSubmission(const model::OrderRequest& request,
                                    Submission& transaction) noexcept;
  // Publishes a validated request, order slot and reservation.
  void CommitSubmission(const model::OrderRequest& request,
                        const Submission& transaction) noexcept;
  // Stages a domain transition plus request, identity and execution checks.
  base::ErrorCode PrepareOrderTransition(
      const model::TradingEvent& event, size_t index, uint64_t transaction_now,
      ReportTransaction& transaction) noexcept;
  // Marks a failed, timed-out or disconnected request as uncertain.
  base::ErrorCode MarkRequestUncertain(
      const OrderExecutionRequest& request) noexcept;
  // Stages cash effects or applies an atomic simulated-futures transaction.
  base::ErrorCode PrepareReportAccount(size_t index,
                                       ReportTransaction& transaction) noexcept;
  // Stages execution cash, holdings and reservation changes.
  base::ErrorCode PrepareCashExecution(size_t index,
                                       ReportTransaction& transaction) noexcept;
  // Publishes a validated report; no remaining operation can fail.
  void CommitReport(size_t index, ReportTransaction& transaction) noexcept;
  // Revisits an already-scanned order on the next bounded stopping pass.
  void ScheduleReportRescan(size_t index) noexcept;
  // Requires no account storage and closed requests. Validates initial cash
  // and distinct, known, unreserved positions before allocating. Failure keeps
  // the account unavailable and preserves caller input.
  [[nodiscard]] base::Status ConfigureAccount(
      const TradingAccountSnapshot& account) noexcept;
  // Validates the complete broker view and monotonic revision without changing
  // retained orders/account or caller input. Financial values remain verbatim.
  [[nodiscard]] base::ErrorCode ValidateBrokerAccount(
      const BrokerAccountSnapshot& account) noexcept;
  // Finds a retained order; IDs are never reused within the session.
  std::optional<size_t> FindIndex(model::ClientOrderId id) const noexcept;
  // Finds a retained position; a known untouched instrument has no slot yet.
  std::optional<size_t> FindPosition(model::InstrumentId id) const noexcept;
  // Atomically reserves queue space and marks an eligible order pending.
  [[nodiscard]] base::ErrorCode QueueCancel(size_t index) noexcept;
  // Copies diagnostics without changing business facts. Detail must not alias
  // this runtime's diagnostic buffer.
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  // Records the adapter stage and diagnostic for a failed lifecycle operation.
  base::Status FailStage(base::Status status, const char* stage,
                         const char* detail) noexcept;

  static constexpr size_t kErrorDetailSize = 1024;
  const base::MonotonicClock& clock_;
  const md::InstrumentScope& instruments_;
  TradingAdapter& adapter_;
  std::unique_ptr<Storage> storage_;
  bool prepared_ = false;
  bool accept_requests_ = false;
  bool stopping_ = false;
  bool prefer_local_ = true;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_ENGINE_H_
