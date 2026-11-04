#ifndef YQUANT_TRADING_TRADING_ADAPTER_H_
#define YQUANT_TRADING_TRADING_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <variant>

#include "yquant/trading/account_recovery.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/order.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trade.h"

namespace yquant::trading {

struct SubmitOrderRequest;

// Records transmission status independently of the counter's decision.
enum class SendResult { kSent, kNotSent };
enum class AdapterOrderState { kAccepted, kRejected, kCanceled };

// Carries the original ID even before the counter assigns its own order ID.
struct AdapterOrderReport {
  OrderId order_id;
  AdapterOrderState state = AdapterOrderState::kAccepted;
  // Total filled quantity reported by the counter, not this event's increment.
  // Rejection requires zero. Cancellation includes the final cumulative fill.
  std::int32_t filled_quantity = 0;
};

// Carries an order ID and the service's cancellation attempt ID to the counter.
struct AdapterCancelOrderRequest {
  OrderId order_id;
  std::uint64_t service_cancel_id = 0;
};

// Preserves both IDs so delayed results cannot complete another attempt.
struct AdapterCancelReport {
  OrderId order_id;
  std::uint64_t service_cancel_id = 0;
  CancelResult result = CancelResult::kCanceled;
  // Final cumulative quantity on successful cancellation. Include fills whose
  // details have not been delivered yet; rejected attempts ignore this field.
  std::int32_t filled_quantity = 0;
};

// Marks the start of a new account funds refresh.
struct FundsRefreshStarted {};
// Ends a refresh without a complete result, including interruption.
struct FundsRefreshFailed {};

// Associates a start, complete funds value or failure with one account refresh.
// Use increasing nonzero IDs throughout the adapter's runtime and reconnects.
// Deliver a start before its result. Assemble the complete funds value before
// sending FundsInfo.
struct AdapterFundsReport {
  std::uint64_t refresh_id = 0;
  std::variant<FundsRefreshStarted, FundsInfo, FundsRefreshFailed> event;
};

// Starts collection of one complete account positions snapshot.
struct PositionsRefreshStarted {};
// Gives the complete normalized row count, determined before data handoff.
struct PositionsRefreshCompleted {
  std::size_t expected_count = 0;
};
// Ends a positions refresh without publishing its partial rows.
struct PositionsRefreshFailed {};

// Carries one positions refresh event on its bound account's business thread.
// IDs increase across the adapter runtime and reconnects. Begin with a start,
// send normalized rows, then complete with the distinct row count or fail.
// Copy data needed after the callback; identical repeated rows keep their key.
struct AdapterPositionsReport {
  std::uint64_t refresh_id = 0;
  std::variant<PositionsRefreshStarted, PositionInfo, PositionsRefreshCompleted,
               PositionsRefreshFailed> event;
};

// Describes the outcome of an initialization attempt. Unresolved orders keep
// the account unavailable until a later attempt can establish a safe baseline.
enum class AccountSyncResult { kComplete, kFailed, kUnresolvedOrders };

// Ends one requested initialization attempt. kComplete certifies that the
// funds and positions delivered with sync_id share a consistent boundary:
// all preceding historical reports are included, and every pre-start order
// that can still affect the account has been recovered. Deliver subsequent live events after this report.
// An adapter unable to prove this boundary must report failure or unresolved
// orders, never infer completion from the arrival order of query responses.
struct AdapterAccountSyncReport {
  std::uint64_t sync_id = 0;
  AccountSyncResult result = AccountSyncResult::kFailed;
};

// Receives reports on the service's business thread.
// Copy report data needed after a callback returns.
class ITradingAdapterCallback {
 public:
  virtual ~ITradingAdapterCallback() = default;
  virtual void OnOrderReport(const AdapterOrderReport& report) = 0;
  virtual void OnCancelReport(const AdapterCancelReport& report) = 0;
  // Preserve source order for distinct fills; duplicate and delayed reports
  // may repeat an existing trade ID without applying its cash impact again.
  virtual void OnTradeReport(const TradeInfo& trade) = 0;
  // Keeps balances and positions unavailable while a received fill is waiting
  // for its native order identity. Clear only when ordered delivery resumes.
  virtual void OnFillRoutingPending(bool pending) = 0;
  // Stages a complete baseline instead of the separate funds/positions streams.
  // Only kComplete for the same sync_id may publish it.
  virtual void OnAccountRecovery(std::uint64_t sync_id, AccountRecovery baseline) = 0;
  // Delivers a post-boundary fill or unsolicited cancellation for a recovered order.
  virtual void OnRecoveredEvent(const RecoveredEvent& event) = 0;
  virtual void OnAccountSyncReport(const AdapterAccountSyncReport& report) = 0;
  virtual void OnFundsReport(const AdapterFundsReport& report) = 0;
  virtual void OnPositionsReport(const AdapterPositionsReport& report) = 0;
};

// Connects one account to its counter. All methods use the business thread.
// Stop adapter callbacks before destroying the registered callback object.
class ITradingAdapter {
 public:
  virtual ~ITradingAdapter() = default;
  // Borrows callback for all subsequent deliveries; register before submitting.
  virtual void SetCallback(ITradingAdapterCallback& callback) = 0;
  // Requests a complete startup baseline using sync_id for both snapshot
  // streams and their final AccountSync report. Returns true when accepted,
  // false when no request was sent. Must not invoke callbacks during this call;
  // deliver all results through Run. Failed attempts may be retried with a new
  // ID; a successful account is not synchronized again during this runtime.
  virtual bool StartAccountSync(std::uint64_t sync_id) = 0;
  // Copies parameters needed after return. Must not invoke any registered
  // Callback during this call. Buffer reports produced by the counter and
  // deliver them after SubmitOrder returns, through Run on the business thread.
  virtual SendResult SubmitOrder(const SubmitOrderRequest& request) = 0;
  // Copies retained data and sends one attempt. Must not invoke Callback during
  // this call; deliver cancellation results through subsequent Run calls.
  virtual SendResult CancelOrder(const AdapterCancelOrderRequest& request) = 0;
  // Delivers available work up to an internal batch limit, then returns.
  virtual void Run() = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ADAPTER_H_
