#ifndef YQUANT_TRADING_LOCAL_TRADING_RUNTIME_H_
#define YQUANT_TRADING_LOCAL_TRADING_RUNTIME_H_

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_limits.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Owns one local trading session: order/account state, reservations, queued
// requests, report commits and stopping progress. All calls are serialized by
// the host. Clock, immutable instrument scope and adapter outlive this object;
// the scope matches Init's dictionary. Init allocates fixed storage once, and
// request/event processing does not allocate. No operation invokes strategies.
class LocalTradingRuntime final : public TradingService {
 public:
  LocalTradingRuntime(const base::MonotonicClock& clock,
                      const md::InstrumentScope& instruments,
                      TradingAdapter& adapter) noexcept;
  ~LocalTradingRuntime() noexcept override;
  LocalTradingRuntime(const LocalTradingRuntime&) = delete;
  LocalTradingRuntime& operator=(const LocalTradingRuntime&) = delete;

  // Starts the adapter and validates/copies its complete initial account.
  // An initial failure leaves requests disabled and is safe to Stop/Reset;
  // kInvalidState preserves an existing session. Reset is required to replace
  // an initialized account. Inputs are call-only.
  [[nodiscard]] base::Status Init(
      const TradingConfig& config,
      const md::MarketDataConfig& dictionary) noexcept override;
  // Closes requests and releases adapter resources, retaining order/account
  // facts until Reset. Idempotent, including after partial initialization.
  [[nodiscard]] base::Status Stop() noexcept override;
  // Stops the adapter, frees storage and invalidates all queries.
  void Reset() noexcept override;
  void SetAcceptingRequests(bool accepting) noexcept override {
    accept_requests_ = accepting && !stopping_;
  }
  // Permanently closes requests and starts a bounded cancellation scan once.
  void BeginStopping() noexcept override;

  bool accepting_orders() const noexcept override;
  [[nodiscard]] base::ErrorCode SubmitOrder(
      const model::OrderRequest& request,
      model::ClientOrderId& out) noexcept override;
  [[nodiscard]] base::ErrorCode CancelOrder(
      model::ClientOrderId id) noexcept override;
  bool TryGetOrder(model::ClientOrderId id,
                   model::OrderSnapshot& out) const noexcept override;
  bool TryGetCash(model::CashSnapshot& out) const noexcept override;
  bool TryGetPosition(model::InstrumentId id,
                      model::PositionSnapshot& out) const noexcept override;

  // Requires requests closed. Marks each attempt before sending; uncertain
  // failures never requeue it. Stopping converts unsent orders to local
  // cancels.
  [[nodiscard]] base::Status ProcessRequests(
      std::uint64_t now, std::size_t limit = 64) noexcept override;
  // Scans a bounded number of slots for cancels, retaining progress on a full
  // queue. Rejected cancels wait at least 10ms before retrying.
  [[nodiscard]] base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool has_ready_event(std::uint64_t now) const noexcept override;
  bool is_drained() const noexcept override;
  std::size_t order_count() const noexcept override;
  const char* error_stage() const noexcept override { return error_stage_; }
  const char* error_detail() const noexcept override { return error_detail_; }
  [[nodiscard]] base::Status Poll(std::uint64_t now, TradingDispatch& out,
                                  bool& event) noexcept override;

  // Commits a supplied normalized report, preserving state and out on failure.
  // Serial access and the runtime's monotonic clock domain are required.
  [[nodiscard]] base::ErrorCode ApplyEvent(const TradingEvent& event,
                                           TradingDispatch& out) noexcept;
  // Commits one queued CanceledBeforeSend event. Empty input returns
  // kInvalidState without changing out or account/order facts.
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(TradingDispatch& out) noexcept;
  bool has_local_updates() const noexcept;

 private:
  struct Storage;
  // Requires no account storage and closed requests. Validates initial cash
  // and distinct, known, unreserved positions before allocating. Failure keeps
  // the account unavailable and preserves caller input.
  [[nodiscard]] base::Status ConfigureAccount(
      const TradingAccountSnapshot& account) noexcept;
  // Finds a retained order; IDs are dense and never reused within this run.
  std::optional<std::size_t> FindIndex(model::ClientOrderId id) const noexcept;
  // Finds a retained position; a known untouched instrument has no slot yet.
  std::optional<std::size_t> FindPosition(
      model::InstrumentId id) const noexcept;
  // Atomically reserves queue space and marks an eligible order pending.
  [[nodiscard]] base::ErrorCode QueueCancel(std::size_t index) noexcept;
  // Copies diagnostics without changing business facts. Detail must not alias
  // this runtime's diagnostic buffer.
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  base::Status Fail(base::Status status, const char* stage,
                    const char* detail) noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  const base::MonotonicClock& clock_;
  const md::InstrumentScope& instruments_;
  TradingAdapter& adapter_;
  Storage* storage_ = nullptr;
  bool prepared_ = false;
  bool accept_requests_ = false;
  bool stopping_ = false;
  bool prefer_local_ = true;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_LOCAL_TRADING_RUNTIME_H_
