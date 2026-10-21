#ifndef YQUANT_TRADING_TRADING_H_
#define YQUANT_TRADING_TRADING_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/callback_phase.h"
#include "yquant/strategy/trading.h"
#include "yquant/trading/trading_account.h"
#include "yquant/trading/trading_limits.h"
#include "yquant/trading/trading_runtime.h"

namespace yquant::trading {
class TradingAdapter;
struct TradingEvent;

// Single-thread order ledger. Large storage is allocated once in Configure;
// all dependencies outlive this owner.
class TradingImpl final : public TradingRuntime {
 public:
  TradingImpl(const base::MonotonicClock& clock,
              const md::InstrumentScope& instruments) noexcept;
  ~TradingImpl() noexcept override;
  TradingImpl(const TradingImpl&) = delete;
  TradingImpl& operator=(const TradingImpl&) = delete;
  // Validates and copies bootstrap before allocating fixed storage. IDs must
  // be known/unique, cash nonnegative, and positions unreserved with
  // 0 <= available <= total. Failure preserves the existing ledger; an initial
  // failure leaves the service disabled.
  [[nodiscard]] base::Status Configure(
      const TradingAccountSnapshot& account) noexcept;
  // Frees owned storage outside callbacks; invalidates order/account queries.
  void Reset() noexcept override;
  void SetRunning(bool running) noexcept override { running_ = running; }
  void BeginCallback(strategy::CallbackPhase phase) noexcept override {
    phase_ = phase;
  }
  void EndCallback() noexcept override {
    phase_ = strategy::CallbackPhase::kNone;
  }
  // Closes request acceptance and starts a bounded cancellation scan once.
  void BeginStopping() noexcept override;

  bool accepting_orders() const noexcept override;
  [[nodiscard]] base::ErrorCode SubmitOrder(
      const model::OrderRequest& request,
      model::ClientOrderId& out) noexcept override;
  [[nodiscard]] base::ErrorCode CancelOrder(
      model::ClientOrderId id) noexcept override;
  [[nodiscard]] bool TryGetOrder(
      model::ClientOrderId id,
      model::OrderSnapshot& out) const noexcept override;

  [[nodiscard]] bool TryGetCash(
      model::CashSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetPosition(
      model::InstrumentId id,
      model::PositionSnapshot& out) const noexcept override;

  // Sends/removes each request at most once, marking attempted before Send.
  // While stopping, new orders become queued local cancellations instead.
  [[nodiscard]] base::Status ProcessRequests(
      TradingAdapter& adapter, std::uint64_t now,
      std::size_t limit = 64) noexcept override;
  // Scans a bounded number of slots for automatic cancels, preserving progress
  // on a full queue. Rejected cancels wait at least 10ms before retrying.
  [[nodiscard]] base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept override;
  // Immediate local work is represented by zero; otherwise the next retry.
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool has_local_updates() const noexcept override;
  bool is_drained() const noexcept override;
  std::size_t order_count() const noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

  // Validates before atomically committing an event. Failure preserves ledger
  // and out, copying a diagnostic. Successful dispatch values own all data.
  [[nodiscard]] base::ErrorCode ApplyEvent(
      const TradingEvent& event, TradingDispatch& out) noexcept override;
  // Commits one queued CanceledBeforeSend event, retaining report sequence.
  // Empty input returns kInvalidState and preserves out and ledger.
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(
      TradingDispatch& out) noexcept override;

 private:
  struct Storage;
  // Finds a retained order; IDs are dense and never reused within this run.
  std::optional<std::size_t> FindIndex(model::ClientOrderId id) const noexcept;
  // Finds a retained position; a known untouched instrument has no slot yet.
  std::optional<std::size_t> FindPosition(
      model::InstrumentId id) const noexcept;
  // Reserves a request and marks a known, eligible order pending atomically.
  // Capacity/ID failure preserves all records and the request queue.
  [[nodiscard]] base::ErrorCode QueueCancel(std::size_t index) noexcept;
  // Stores host-owned diagnostics without changing business state.
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;

  const base::MonotonicClock& clock_;
  const md::InstrumentScope& instruments_;
  Storage* storage_ = nullptr;
  bool running_ = false;
  bool stopping_ = false;
  strategy::CallbackPhase phase_ = strategy::CallbackPhase::kNone;
  char error_detail_[256] = {};
};
}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_H_
