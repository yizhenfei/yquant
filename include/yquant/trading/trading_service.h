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
#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/order.h"
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

enum class RequestResult { kDelivered, kIdExhausted };
enum class FailureReason {
  kUnknownAccount,
  kDuplicateOrder,
  kInvalidRequest,
  kSendFailed,
  kUnknownOrder,
  kOwnershipMismatch,
  kOrderInactive,
  kDuplicateCancel,
  kIdExhausted,
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

  // container::Service implementation.
  // Delivers a bounded notification batch, then advances each account once.
  // Returns immediately during a request, a callback or an existing Run.
  void Run() override;

 private:
  // Carries the bound account into every callback without a downcast or lookup.
  struct AccountContext final : ITradingAdapterCallback {
    AccountContext(TradingService& owner, std::uint64_t id,
                   ITradingAdapter& adapter)
        : owner(owner), id(id), adapter(adapter) {}

    // ITradingAdapterCallback implementation.
    void OnOrderReport(const AdapterOrderReport& report) override;
    void OnCancelReport(const AdapterCancelReport& report) override;
    void OnTradeReport(const TradeInfo& trade) override;

    TradingService& owner;
    std::uint64_t id;
    ITradingAdapter& adapter;
    std::unordered_map<TradeId, TradeInfo, TradeIdHash> trades;
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
