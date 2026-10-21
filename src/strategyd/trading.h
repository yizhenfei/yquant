#ifndef YQUANT_STRATEGYD_TRADING_H_
#define YQUANT_STRATEGYD_TRADING_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "strategyd/market_data.h"
#include "strategyd/trading_account.h"
#include "strategyd/trading_limits.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/trading.h"

namespace yquant::strategyd {
class TradingAdapter;
struct TradingEvent;

// One fully committed source/local event, copied for serial callback dispatch.
// An old source event succeeds with both notification flags false.
struct TradingDispatch {
  model::OrderUpdate update;
  model::Fill fill;
  bool notify_order = false;
  bool has_fill = false;
};

// Single-thread order ledger. Large storage is allocated once in Configure;
// the small owner is embedded in Runner. All dependencies outlive this owner.
class TradingImpl final : public strategy::Trading {
 public:
  TradingImpl(const MonotonicClock& clock,
              const MarketDataImpl& market_data) noexcept;
  ~TradingImpl() noexcept;
  TradingImpl(const TradingImpl&) = delete;
  TradingImpl& operator=(const TradingImpl&) = delete;
  // Validates and copies bootstrap before allocating fixed storage. IDs must
  // be known/unique, cash nonnegative, and positions unreserved with
  // 0 <= available <= total, with all quantities aligned to quantity_step.
  // Futures use four directional/day FIFO buckets and
  // simulated gross margin; the account cannot mix equity/futures inventory.
  // Failure preserves the existing ledger; initial failure leaves it disabled.
  [[nodiscard]] base::Status Configure(
      const TradingAccountSnapshot& account) noexcept;
  // Frees owned storage outside callbacks; invalidates order/account queries.
  void Reset() noexcept;
  void SetRunning(bool running) noexcept { running_ = running; }
  void BeginCallback(CallbackPhase phase) noexcept { phase_ = phase; }
  void EndCallback() noexcept { phase_ = CallbackPhase::kNone; }
  // Closes request acceptance and starts a bounded cancellation scan once.
  void BeginStopping() noexcept;

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
  [[nodiscard]] bool TryGetFuturesAccount(
      model::FuturesAccountSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetFuturesPosition(
      model::InstrumentId id,
      model::FuturesPositionSnapshot& out) const noexcept override;

  // Marks an accepted quote before its strategy callback. Disabled/cash
  // accounts need no valuation and succeed. Futures failures preserve the
  // entire account/position and copy a diagnostic; incomplete quotes retain
  // the previous mark. Never invokes callbacks or allocates.
  [[nodiscard]] base::ErrorCode ApplyQuote(
      const model::QuoteTick& tick) noexcept;

  // Sends/removes each request at most once, marking attempted before Send.
  // While stopping, new orders become queued local cancellations instead.
  [[nodiscard]] base::Status ProcessRequests(TradingAdapter& adapter,
                                             std::uint64_t now,
                                             std::size_t limit = 64) noexcept;
  // Scans a bounded number of slots for automatic cancels, preserving progress
  // on a full queue. Rejected cancels wait at least 10ms before retrying.
  [[nodiscard]] base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept;
  // Immediate local work is represented by zero; otherwise the next retry.
  std::optional<std::uint64_t> next_deadline() const noexcept;
  bool has_local_updates() const noexcept;
  bool is_drained() const noexcept;
  std::size_t order_count() const noexcept;
  const char* error_detail() const noexcept { return error_detail_; }

  // Validates before atomically committing an event. Failure preserves ledger
  // and out, copying a diagnostic. Successful dispatch values own all data.
  [[nodiscard]] base::ErrorCode ApplyEvent(const TradingEvent& event,
                                           TradingDispatch& out) noexcept;
  // Commits one queued CanceledBeforeSend event, retaining report sequence.
  // Empty input returns kInvalidState and preserves out and ledger.
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(TradingDispatch& out) noexcept;

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

  const MonotonicClock& clock_;
  const MarketDataImpl& market_data_;
  Storage* storage_ = nullptr;
  bool running_ = false;
  bool stopping_ = false;
  CallbackPhase phase_ = CallbackPhase::kNone;
  char error_detail_[256] = {};
};
}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_TRADING_H_
