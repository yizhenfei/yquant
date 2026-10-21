#ifndef YQUANT_STRATEGYD_TRADING_H_
#define YQUANT_STRATEGYD_TRADING_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "strategyd/market_data.h"
#include "strategyd/trading_account.h"
#include "strategyd/trading_limits.h"
#include "strategyd/trading_persistence.h"
#include "strategyd/trading_runtime.h"
#include "yquant/model/trading_event.h"
#include "yquant/strategy/trading.h"

namespace yquant::strategyd {
class TradingAdapter;
struct TradingEvent;

// Single-thread order ledger. Large storage is allocated once in Configure;
// the small owner is embedded in Runner. All dependencies outlive this owner.
class TradingImpl final : public TradingRuntime {
 public:
  TradingImpl(const MonotonicClock& clock,
              const MarketDataImpl& market_data) noexcept;
  ~TradingImpl() noexcept override;
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
  void Reset() noexcept override;
  void SetRunning(bool running) noexcept override { running_ = running; }
  void BeginCallback(CallbackPhase phase) noexcept override { phase_ = phase; }
  void EndCallback() noexcept override { phase_ = CallbackPhase::kNone; }
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
      const model::QuoteTick& tick) noexcept override;

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

  // Installs a borrowed main-thread sink on a configured cash ledger. It must
  // outlive ledger work. nullptr disables it, including on an empty ledger.
  // Call outside callbacks/replay; failure preserves the existing sink.
  // Futures persistence is unsupported and rejected before any mutation.
  [[nodiscard]] base::Status SetMutationSink(
      TradingMutationSink* sink) noexcept;
  // Cash journal revision; futures execution leaves this zero.
  std::uint64_t mutation_revision() const noexcept;
  std::uint64_t next_order_id() const noexcept;
  std::uint64_t next_cancel_id() const noexcept;
  // Cold/bounded retained-slot iteration; false preserves out.
  [[nodiscard]] bool TryGetOrderAt(std::size_t index,
                                   model::OrderSnapshot& out) const noexcept;
  std::size_t fill_count() const noexcept;
  // Cold retained execution iteration; false preserves out. Access must be
  // serialized with ledger mutations, matching the order iteration contract.
  [[nodiscard]] bool TryGetFillAt(std::size_t index,
                                model::Fill& out) const noexcept;
  // Cold identity allocation: never lowers the next ID, allocates no storage,
  // and changes no existing order. The owner persists its lease before use.
  [[nodiscard]] base::Status SetNextOrderIdAtLeast(std::uint64_t id) noexcept;
  // Same cold lease contract for cancel IDs; id is the next assignable value.
  [[nodiscard]] base::Status SetNextCancelIdAtLeast(std::uint64_t id) noexcept;
  // Cash-only cold codec operations; futures return kUnsupported.
  // Export does not allocate and preserves size on failure. Restore validates
  // a private candidate and preserves this ledger
  // on failure; successful restore leaves execution stopped and callbacks off.
  // The checkpoint includes pending queues, reservations and dedup identities.
  [[nodiscard]] base::Status ExportCheckpoint(std::span<std::byte> bytes,
                                              std::size_t& size) const noexcept;
  [[nodiscard]] base::Status RestoreCheckpoint(
      std::span<const std::byte> bytes) noexcept;
  // Installs an independently reconciled cash baseline without financially
  // applying historical executions again. All supplied orders must be terminal
  // with no outstanding cancels; every cumulative fill needs its unique detail.
  // Baseline reservations must be zero; sellable quantities are preserved.
  // Orders may be unordered, but IDs must be unique, nonzero and below
  // UINT64_MAX. Cold-only and failure atomic. Existing ID leases are never
  // lowered; the mutation revision starts at zero for the new recovery
  // baseline.
  [[nodiscard]] base::Status RestoreReconciled(
      const TradingAccountSnapshot& baseline,
      std::span<const model::OrderSnapshot> terminal_orders,
      std::span<const model::Fill> executions) noexcept;
  // Cash-only: replays the next fact without sending, invoking the sink or
  // notifying a strategy. Failure preserves business state. Call only while
  // stopped, outside callbacks; time-dependent validation uses the fact's now.
  [[nodiscard]] base::Status ReplayMutation(
      const TradingMutation& mutation, TradingDispatch* out = nullptr) noexcept;
  // Cold recovery boundary: conservatively marks restored queued requests as
  // attempted and removes them. No external request is sent. The owner must
  // reconcile these uncertain orders before enabling execution.
  void AbandonRecoveredRequests() noexcept;

 private:
  struct Storage;
  // Publishes the next validated fact. Successful commit callers increment
  // mutation_revision exactly once; failed callbacks preserve business state.
  [[nodiscard]] base::ErrorCode BeforeCommit(TradingMutation& fact) noexcept;
  // Finds a retained order independently of its storage slot.
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
  TradingMutationSink* sink_ = nullptr;
  bool replaying_ = false;
  bool invoking_sink_ = false;
  std::uint64_t replay_now_ = 0;
};
}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_TRADING_H_
