#ifndef YQUANT_TRADING_TRADING_SERVICE_H_
#define YQUANT_TRADING_TRADING_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

#include "yquant/container/service.h"
#include "yquant/trading/internal/call_context.h"
#include "yquant/trading/order.h"
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

enum class RequestResult { kDelivered, kIdExhausted };
enum class FailureReason {
  kUnknownAccount,
  kDuplicateOrder,
  kInvalidRequest,
  kSendFailed,
};

// Reports a processing or transmission error for order_id.
// Validation failures have no snapshot and leave existing orders unchanged.
// Transmission failures include the established order's state snapshot.
struct RequestFailure {
  OrderId order_id;
  FailureReason reason = FailureReason::kInvalidRequest;
  std::optional<OrderSnapshot> snapshot;
};

// Owns the state at this event's version, independent of later state changes.
struct OrderReport {
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

    TradingService& owner;
    std::uint64_t id;
    ITradingAdapter& adapter;
  };
  // Stores state once for both notifications and subsequent queries.
  struct OrderRecord {
    OrderInfo order;
    TradingListener* listener;
    internal::CallContext call;
    OrderState state = OrderState::kSubmitting;
    std::int32_t filled_quantity = 0;
    std::uint64_t version = 1;
    std::size_t pending = 0;
  };
  // Keeps an event's snapshot and internal invocation context together.
  struct Notification {
    TradingListener* listener = nullptr;
    std::variant<RequestFailure, OrderReport> event;
    OrderRecord* order = nullptr;
    internal::CallContext call;
  };
  // Validates and submits an order. SubmitOrder keeps request_depth_ positive
  // during this call so notifications wait until the request method returns.
  void ProcessSubmitOrder(const SubmitOrderRequest& request,
                          TradingListener& listener,
                          internal::CallContext call);
  // Copies the current order record into an independent value for reporting.
  static OrderSnapshot MakeSnapshot(const OrderRecord& order);
  // Validates the source account and applies one accepted/rejected event.
  void Apply(std::uint64_t account_id, const AdapterOrderReport& report);
  // Saves a request outcome; record is null when no order was established.
  void Fail(OrderId order_id, TradingListener& listener,
            internal::CallContext call, FailureReason reason,
            OrderRecord* record = nullptr);
  // Queues events during a request or behind an order's pending events.
  // Delivers other events immediately.
  void Notify(Notification notification);
  // Invokes the listener with an independent snapshot and guards nested Run.
  void Dispatch(const Notification& notification);

  std::vector<std::unique_ptr<AccountContext>> accounts_;
  std::unordered_map<std::uint64_t, AccountContext*> account_index_;
  std::unordered_map<OrderId, OrderRecord, OrderIdHash> orders_;
  std::vector<Notification> pending_;
  std::size_t head_ = 0;
  std::size_t pending_count_ = 0;
  std::size_t request_depth_ = 0;
  std::size_t callback_depth_ = 0;
  bool running_ = false;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_SERVICE_H_
