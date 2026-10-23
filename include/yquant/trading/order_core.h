#ifndef YQUANT_TRADING_ORDER_CORE_H_
#define YQUANT_TRADING_ORDER_CORE_H_

// Defines a single-order business state machine and its events and actions.
// OrderRequestExecutor handles request execution; TradingEngine applies account
// changes and coordinates transaction commits.

#include <stddef.h>
#include <stdint.h>

#include <array>

#include "yquant/base/status.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {

// Specifies the trading rules that apply to an order. OrderCore uses them
// to validate incoming events. The rules do not change as events are processed.
struct OrderCoreRules {
  int64_t price_tick = 1;
  int64_t quantity_step = 1;
  bool allow_unsolicited_cancel = false;
  friend bool operator==(const OrderCoreRules&,
                         const OrderCoreRules&) noexcept = default;
};

enum class OrderEventKind : uint8_t {
  kCreated,
  kCancelRequested,
  kBrokerReport,
  kCanceledBeforeSend,
  kCancelNotSent,
};

// Describes a business event. Created uses request/rules, BrokerReport uses
// report, and local cancel events use cancel_request_id/reason. Every event
// carries its client_order_id and the transaction's monotonic processing time.
// The executor validates send progress before forwarding reports or results.
struct OrderEvent {
  OrderEventKind kind = OrderEventKind::kCreated;
  model::ClientOrderId client_order_id;
  uint64_t now = 0;
  model::OrderRequest request;
  OrderCoreRules rules;
  model::CancelRequestId cancel_request_id;
  model::TradingEvent report;
  model::OrderReason reason;
};

enum class OrderActionKind : uint8_t {
  kSubmitOrder,
  kCancelOrder,
  kApplyExecution,
  kReleaseRemainingReservation,
  kPublishOrderUpdate,
};

// Distinguishes an accepted fact, including a silent late acceptance, from an
// ignored old broker sequence. OrderApplyResult::code represents errors.
enum class OrderApplyOutcome : uint8_t { kApplied, kIgnored };

// One report can apply/release resources and publish one order update.
inline constexpr size_t kMaxOrderActions = 2;

// Reports an event's outcome and actions. Only actions[0..action_count)
// execute; request supplies SubmitOrder, fill supplies ApplyExecution, and
// update supplies the order/attempt identity for every action. update.order is
// the resulting snapshot even for silent acceptance. Errors return no actions.
struct [[nodiscard]] OrderApplyResult {
  base::ErrorCode code = base::ErrorCode::kOk;
  OrderApplyOutcome outcome = OrderApplyOutcome::kApplied;
  std::array<OrderActionKind, kMaxOrderActions> actions = {};
  size_t action_count = 0;
  model::OrderRequest request;
  model::OrderUpdate update;
  model::Fill fill;
  model::OrderReason diagnostic;
  bool ok() const noexcept { return code == base::ErrorCode::kOk; }
};

struct OrderRestoreResult;

// Maintains one order's business state. Apply is failure atomic and does not
// allocate. Copies form independent candidate states; the host coordinates
// their publication with account, index and WAL changes. Serialize access to
// an instance; the core has no internal lock.
class OrderCore final {
 public:
  OrderCore() noexcept = default;

  // Applies a local or broker event and returns its actions. The first
  // successful event must be Created. Invalid inputs leave this core unchanged.
  [[nodiscard]] OrderApplyResult Apply(const OrderEvent& event) noexcept;

  // Validates and installs a snapshot without producing actions. The host also
  // checks execution history, request progress and account/index consistency.
  [[nodiscard]] static OrderRestoreResult Restore(
      const model::OrderSnapshot& snapshot,
      const OrderCoreRules& rules) noexcept;

  bool initialized() const noexcept { return initialized_; }
  // Returns the current snapshot; valid only after creation or restore.
  const model::OrderSnapshot& snapshot() const noexcept { return snapshot_; }
  const OrderCoreRules& rules() const noexcept { return rules_; }

 private:
  // Builds a new order from a locally accepted request without external checks.
  OrderApplyResult Create(const OrderEvent& event) noexcept;
  // Registers an eligible local cancellation without changing the lifecycle.
  OrderApplyResult RequestCancel(const OrderEvent& event) noexcept;
  // Applies an executor-certified local completion without advancing reports.
  OrderApplyResult ApplyLocalResult(const OrderEvent& event) noexcept;
  // Validates report fields, filters old sequences, then stages a transition.
  OrderApplyResult ApplyReport(const OrderEvent& event) noexcept;

  model::OrderSnapshot snapshot_;
  OrderCoreRules rules_;
  bool initialized_ = false;
};

// Reports validation of a restored core. On error, core is uninitialized.
struct [[nodiscard]] OrderRestoreResult {
  base::ErrorCode code = base::ErrorCode::kOk;
  OrderCore core;
  model::OrderReason diagnostic;
  bool ok() const noexcept { return code == base::ErrorCode::kOk; }
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_ORDER_CORE_H_
