#ifndef YQUANT_TRADING_TRADING_SERVICE_H_
#define YQUANT_TRADING_TRADING_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "yquant/container/service.h"
#include "yquant/trading/funds.h"
#include "yquant/trading/funds_terms.h"
#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/internal/funds_calculation.h"
#include "yquant/trading/order.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trade.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_listener.h"

namespace yquant::trading {

// Carries order information for a submission request.
// Once an order record is established, it owns a copy of order.
// Callers can reuse this request after SubmitOrder returns.
struct SubmitOrderRequest {
  OrderInfo order;
  bool operator==(const SubmitOrderRequest&) const = default;
};

// Identifies one cancellation attempt and the order's account and strategy.
struct CancelOrderRequest {
  OrderId order_id;
  std::uint64_t account_id = 0;
  std::uint64_t strategy_id = 0;
  std::uint64_t cancel_request_id = 0;
};

// Selects an order and the account and strategy that must own it.
struct QueryOrderRequest {
  OrderId order_id;
  std::uint64_t account_id = 0;
  std::uint64_t strategy_id = 0;
};

// Selects the account whose shared funds state the caller wants to read.
struct QueryFundsRequest {
  std::uint64_t account_id = 0;
};

// Selects the shared account positions view.
struct QueryPositionsRequest {
  std::uint64_t account_id = 0;
};
// Selects one holding within an account's complete positions view.
struct QueryPositionRequest {
  std::uint64_t account_id = 0;
  PositionKey key;
};

enum class RequestResult { kDelivered, kIdExhausted };
enum class FailureReason {
  kUnknownAccount,
  kAccountNotReady,
  kDuplicateOrder,
  kInvalidRequest,
  kSendFailed,
  kUnknownOrder,
  kOwnershipMismatch,
  kOrderInactive,
  kDuplicateCancel,
  kIdExhausted,
  kFundsUnavailable,
  kFundsParametersUnavailable,
  kCurrencyMismatch,
  kInsufficientFunds,
  kInvalidFundsRequirement,
  kInsufficientPosition,
};

// Reports a processing or transmission error for order_id.
// Validation failures have no snapshot and leave existing orders unchanged.
// Transmission failures include the established order's state snapshot.
struct RequestFailure {
  OrderId order_id;
  FailureReason reason = FailureReason::kInvalidRequest;
  std::optional<OrderSnapshot> snapshot;
  // Present for cancellation failures, including an invalid request ID of zero.
  std::optional<std::uint64_t> cancel_request_id = std::nullopt;
};

// Owns the state at this event's version, independent of later state changes.
struct OrderReport {
  OrderSnapshot snapshot;
};

// Reports one cancellation's counter result and the current order state.
struct CancelReport {
  OrderId order_id;
  std::uint64_t cancel_request_id;
  CancelResult result;
  OrderSnapshot snapshot;
};

// Contains one newly received trade and the resulting order snapshot.
// A late trade is delivered even when the order is terminal. If the order state
// and total filled quantity stay unchanged, the version also stays unchanged.
struct TradeReport {
  TradeInfo trade;
  OrderSnapshot snapshot;
};

// Routes orders to borrowed account adapters and owns their business state.
// Use on one business thread. Adapters and listeners must outlive service use;
// stop adapter callbacks before destroying this service's account contexts.
class TradingService final : public container::Service {
 public:
  TradingService();
  TradingService(const TradingService&) = delete;
  TradingService& operator=(const TradingService&) = delete;

  // Binds during initialization. Duplicate accounts or adapter instances fail
  // without replacing the existing binding. Borrows adapter until shutdown.
  bool BindAccount(std::uint64_t account_id, ITradingAdapter& adapter);
  // Reports startup completion; unknown accounts return false.
  // QueryFunds separately reports runtime accounting availability.
  bool IsAccountReady(std::uint64_t account_id) const;
  // Schedules another attempt after a failed initialization. Returns false
  // for unknown, pending or ready accounts, or from requests/Run/callbacks.
  // The next Run sends the request; this call does not invoke the adapter.
  bool RetryAccountSync(std::uint64_t account_id);
  // Copies supplied rates during assembly and returns true.
  // Returns false without changing terms for invalid input, an unknown account,
  // or calls during Run, requests, callbacks or after any order is established.
  // Missing rates stay unknown; finish all configuration before trading.
  bool ConfigureFundsTerms(std::uint64_t account_id,
                           std::uint32_t instrument_id, Side side,
                           const FundsTerms& terms);
  // Copies data needed after return and retains listener for reporting.
  // Validation and transmission outcomes arrive after return.
  // Entry point for clients and server adapters.
  RequestResult SubmitOrder(const SubmitOrderRequest& request,
                            TradingListener& listener,
                            internal::CallContext call);
  // Copies data needed after return and retains listener for reporting.
  // A sent cancellation leaves the order active until the counter confirms it.
  RequestResult CancelOrder(const CancelOrderRequest& request,
                            TradingListener& listener,
                            internal::CallContext call);
  // Copies the current snapshot and returns true for an existing, owned order.
  // Returns false without changing snapshot if the ID or ownership mismatches.
  bool QueryOrder(const QueryOrderRequest& query,
                  OrderSnapshot& snapshot) const;

  // Copies the latest available complete funds snapshot and returns true.
  // Returns false if the account is unknown or its funds are unavailable.
  // Leaves snapshot unchanged on false. Values become available only after
  // initialization completes and all known fills are booked without errors.
  bool QueryFunds(const QueryFundsRequest& query,
                  FundsSnapshot& snapshot) const;

  // Returns true and borrows the complete rows when accounting is available.
  // Copy rows before the next position update or service destruction.
  // Returns false without changing snapshot if the account is unknown or its
  // positions are unavailable.
  bool QueryPositions(const QueryPositionsRequest& query,
                      PositionsSnapshot& snapshot) const;
  // Copies one holding, or zero quantities for an absent valid key, on true.
  // Returns false for unknown or unavailable accounts and invalid keys.
  // Leaves position unchanged on false.
  bool QueryPosition(const QueryPositionRequest& query,
                     PositionInfo& position) const;

  // container::Service implementation.
  // Delivers a bounded notification batch, then advances each account once.
  // Starts account initialization on its first Run and after an explicit retry.
  // Returns immediately during a request, a callback or an existing Run.
  void Run() override;

 private:
  // Keeps row order contiguous and locates a holding by its complete identity.
  struct PositionsBook {
    std::vector<PositionInfo> rows;
    std::unordered_map<PositionKey, std::size_t, PositionKey::Hash> index;
  };
  enum class SyncState { kNotStarted, kPending, kReady, kFailed };
  // Carries the bound account into every callback without a downcast or lookup.
  struct AccountContext final : ITradingAdapterCallback {
    AccountContext(TradingService& owner, std::uint64_t id,
                   ITradingAdapter& adapter)
        : owner(owner), id(id), adapter(adapter) {}

    // ITradingAdapterCallback implementation.
    void OnOrderReport(const AdapterOrderReport& report) override;
    void OnCancelReport(const AdapterCancelReport& report) override;
    void OnTradeReport(const TradeInfo& trade) override;
    void OnAccountSyncReport(const AdapterAccountSyncReport& report) override;
    void OnFundsReport(const AdapterFundsReport& report) override;
    void OnPositionsReport(const AdapterPositionsReport& report) override;

    TradingService& owner;
    std::uint64_t id;
    ITradingAdapter& adapter;
    std::unordered_map<TradeId, TradeInfo, TradeIdHash> trades;
    SyncState sync_state = SyncState::kNotStarted;
    std::uint64_t sync_id = 0;
    FundsSnapshot funds;
    std::unordered_map<std::uint64_t, FundsTerms> funds_terms;
    long double reserved_funds = 0;
    long double reserved_margin = 0;
    std::size_t reservation_count = 0;
    std::size_t unpriced_orders = 0;
    std::uint64_t funds_refresh_id = 0;
    bool funds_refresh_pending = false;
    bool funds_available = false;
    PositionsBook positions;
    PositionsBook positions_staging;
    std::uint64_t positions_refresh_id = 0;
    std::uint64_t positions_snapshot_id = 0;
    bool positions_refresh_pending = false;
    bool positions_available = false;
  };
  // Pins a close order to its original today/yesterday allocation. Row indices
  // stay valid when the position book grows because it never removes rows.
  struct CloseHold {
    std::size_t row = 0;
    bool today_first = false;
    std::int32_t first_quantity = 0;
    // Contracts already booked plus contracts still frozen for this order.
    // Cancellation reduces this count to the confirmed fill quantity.
    std::int32_t held_end = 0;
    // Counts today's contracts in the first n contracts of this order.
    std::int32_t TodayPrefix(std::int32_t n) const;
  };
  // Stores state once for both notifications and subsequent queries.
  struct OrderRecord {
    OrderInfo order;
    TradingListener* listener;
    internal::CallContext call;
    OrderState state = OrderState::kSubmitting;
    std::int32_t reported_filled_quantity = 0;
    std::int32_t detailed_filled_quantity = 0;
    std::uint64_t version = 1;
    std::size_t pending = 0;
    double reserved_funds = 0;
    // Retains the original estimate for acceptance after a failed send.
    double initial_reservation = 0;
    double initial_margin = 0;
    double reserved_margin = 0;
    std::int32_t booked_quantity = 0;
    bool funds_pending = false;
    CloseHold close{};
    std::unordered_set<std::uint64_t> cancel_ids{};
  };
  // Keeps each attempt independent of other cancellations for the same order.
  struct CancelRecord {
    CancelOrderRequest request;
    OrderRecord* order;
    TradingListener* listener;
    internal::CallContext call;
    bool send_failed = false;
    std::optional<CancelResult> result = std::nullopt;
    std::int32_t final_filled_quantity = 0;
  };
  // Keeps an event's snapshot and internal invocation context together.
  struct Notification {
    TradingListener* listener = nullptr;
    std::variant<RequestFailure, OrderReport, CancelReport, TradeReport> event;
    OrderRecord* order = nullptr;
    internal::CallContext call;
  };
  // Validates and submits an order. SubmitOrder keeps request_depth_ positive
  // during this call so notifications wait until the request method returns.
  void ProcessSubmitOrder(const SubmitOrderRequest& request,
                          TradingListener& listener,
                          internal::CallContext call);
  // Reads account state and terms without mutation; on success writes amount.
  // On failure returns the request error used by the deferred failure callback.
  std::optional<FailureReason> CheckFunds(const AccountContext& account,
                                         const OrderInfo& order,
                                         internal::OrderCost& cost) const;
  // Finds immutable configured terms for an account/order, or returns nullptr.
  const FundsTerms* FindFundsTerms(const AccountContext& account,
                                  const OrderInfo& order) const;
  // Applies one unique fill to a candidate position and commits all cash and
  // cost changes together. On failure, retains balances and blocks cash use.
  void BookTrade(AccountContext& account, OrderRecord& order,
                 const TradeInfo& trade);
  // After funds validation, freezes the available contracts needed for a close.
  // Returns false without changing either bucket if the holding is insufficient.
  bool FreezeClose(AccountContext& account, const OrderInfo& order,
                   CloseHold& hold);
  // Releases or restores this order's own allocation after a state change.
  // If a late report needs contracts since used elsewhere, blocks account use.
  void UpdateCloseHold(AccountContext& account, OrderRecord& order);
  // Updates the account reservation from this order's latest confirmed state.
  // Canceled filled quantities retain their estimate until priced by a trade.
  void UpdateReservation(AccountContext& account, OrderRecord& order);
  // Validates and sends one attempt while CancelOrder defers notifications.
  void ProcessCancelOrder(const CancelOrderRequest& request,
                          TradingListener& listener,
                          internal::CallContext call);
  // Applies a counter result to its attempt and updates the order on success.
  void ApplyCancel(std::uint64_t account_id, const AdapterCancelReport& report);
  // Validates a detail and deduplicates it within its bound account.
  void ApplyTrade(AccountContext& account, const TradeInfo& trade);
  // Saves counter and detail totals. Returns true and increments the version
  // only if the order state or total filled quantity changes.
  static bool UpdateFill(OrderRecord& order, OrderState state,
                         std::int32_t reported, std::int32_t detailed);
  // Finds an account-owned order or diagnoses an unassociated report.
  OrderRecord* FindReportedOrder(std::uint64_t account_id, OrderId id);
  // Starts a queued initialization attempt and resets its partial results.
  void StartAccountSync(AccountContext& account);
  // Closes an attempt and keeps new orders blocked; logs the failure reason.
  void FailAccountSync(AccountContext& account, const char* reason);
  // Publishes complete funds for the active refresh or marks funds unavailable.
  // Retains the last complete value and ignores superseded or closed rounds.
  void ApplyFunds(AccountContext& account, const AdapterFundsReport& report);
  // Collects the current refresh without exposing partial rows to queries.
  void ApplyPositions(AccountContext& account,
                      const AdapterPositionsReport& report);
  // Validates and stages a unique row, ignoring identical duplicate rows.
  static void StagePosition(AccountContext& account,
                            const PositionInfo& position);
  // Publishes a complete row set by swapping the staging and published storage.
  static void CompletePositions(AccountContext& account, std::size_t count);
  // Closes a failed refresh and makes queries unavailable, retaining old rows.
  static void FailPositions(AccountContext& account, const char* reason);
  // Copies the current order record into an independent value for reporting.
  static OrderSnapshot MakeSnapshot(const OrderRecord& order);
  // Validates and applies a counter state and cumulative quantity.
  void Apply(std::uint64_t account_id, const AdapterOrderReport& report);
  // Writes the account and complete order identity for an invalid event.
  static void Diagnose(const char* message, std::uint64_t account_id,
                       OrderId id);
  // Saves a request failure. A record adds its snapshot and associates the
  // notification with that order's pending events. Use null for validation
  // failures.
  void Fail(OrderId order_id, TradingListener& listener,
            internal::CallContext call, FailureReason reason,
            OrderRecord* record = nullptr,
            std::optional<std::uint64_t> cancel_id = std::nullopt);
  // Queues events during a request or behind an order's pending events.
  // Delivers other events immediately.
  void Notify(Notification notification);
  // Invokes the listener with an independent snapshot and guards nested Run.
  void Dispatch(const Notification& notification);

  std::vector<std::unique_ptr<AccountContext>> accounts_;
  std::unordered_map<std::uint64_t, AccountContext*> account_index_;
  std::unordered_map<OrderId, OrderRecord, OrderIdHash> orders_;
  std::unordered_map<std::uint64_t, CancelRecord> cancels_;
  std::uint64_t last_cancel_id_ = 0;
  std::vector<Notification> pending_;
  std::size_t head_ = 0;
  std::size_t pending_count_ = 0;
  std::size_t request_depth_ = 0;
  std::size_t callback_depth_ = 0;
  bool running_ = false;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_SERVICE_H_
