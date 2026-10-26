#ifndef YQUANT_TRADING_ORDER_REQUEST_EXECUTOR_H_
#define YQUANT_TRADING_ORDER_REQUEST_EXECUTOR_H_

// Tracks transport progress independently of order business state. The host
// stages one request transition, then commits without sending.

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>

#include "yquant/base/status.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {

// Allows one minute for a newly attempted request to receive a broker result.
inline constexpr uint64_t kDefaultOrderResponseTimeoutNs = 60000000000;

enum class OrderRequestKind : uint8_t { kNew, kCancel };
enum class OrderRequestState : uint8_t {
  kQueued,
  kInFlight,
  kCompleted,
  kUncertain,
};
enum class OrderLocalResultKind : uint8_t {
  kCanceledBeforeSend,
  kCancelNotSent,
};

// Retains send-attempt information after a request completes.
struct OrderRequestProgress {
  OrderRequestState state = OrderRequestState::kCompleted;
  bool attempted = false;
  uint64_t attempted_at_ns = 0;
  // Zero means that this request has no active response deadline.
  uint64_t response_deadline_ns = 0;
  friend bool operator==(const OrderRequestProgress&,
                         const OrderRequestProgress&) = default;
};

// Tracks the new request and latest cancel for one dense order slot. A zero
// client ID denotes an unused slot; zero cancel ID denotes no cancel attempt.
struct RequestExecution {
  model::ClientOrderId client_order_id;
  OrderRequestProgress new_request;
  model::CancelRequestId cancel_request_id;
  OrderRequestProgress cancel_request;
  bool new_local_result_pending = false;
  bool cancel_local_result_pending = false;
  friend bool operator==(const RequestExecution&,
                         const RequestExecution&) = default;
};

// Identifies a new request or a specific cancel attempt. cancel_request_id is
// zero for a new request.
struct OrderExecutionRequest {
  OrderRequestKind kind = OrderRequestKind::kNew;
  size_t order_index = 0;
  model::ClientOrderId client_order_id;
  model::CancelRequestId cancel_request_id;
  friend bool operator==(const OrderExecutionRequest&,
                         const OrderExecutionRequest&) = default;
};

// Remains queued until its business event is committed with the order core.
struct OrderExecutionLocalResult {
  OrderLocalResultKind kind = OrderLocalResultKind::kCanceledBeforeSend;
  OrderExecutionRequest request;
  uint64_t time_ns = 0;
  friend bool operator==(const OrderExecutionLocalResult&,
                         const OrderExecutionLocalResult&) = default;
};

// Describes a candidate change to one order and at most one entry per queue.
// Only successful candidates may be committed. It expires on the next executor
// mutation; the host serializes preparation and commit without reentry.
struct OrderRequestExecutionTransition {
  base::ErrorCode code = base::ErrorCode::kOk;
  const char* detail = "";
  size_t order_index = 0;
  RequestExecution next;
  std::optional<OrderExecutionRequest> enqueue_request;
  std::optional<OrderExecutionRequest> remove_request;
  std::optional<OrderExecutionLocalResult> append_result;
  bool consume_result = false;
  bool register_order = false;
  uint64_t generation = 0;
  [[nodiscard]] bool ok() const noexcept {
    return code == base::ErrorCode::kOk;
  }
};

// Tracks execution using fixed request and local-result queues. All calls are
// serialized on the host thread. Prepare methods are read-only and allocate
// nothing; Commit applies a current successful candidate without failure.
class OrderRequestExecutor final {
 public:
  // Queue entries are logical ring contents beginning at each head; unused
  // entries are ignored.
  struct State {
    std::array<RequestExecution, kMaxTradingOrders> executions = {};
    size_t order_count = 0;
    std::array<OrderExecutionRequest, kMaxTradingRequests> requests = {};
    size_t request_head = 0;
    size_t request_count = 0;
    std::array<OrderExecutionLocalResult, kMaxLocalOrderUpdates> local_results =
        {};
    size_t local_head = 0;
    size_t local_count = 0;
    uint64_t response_timeout_ns = kDefaultOrderResponseTimeoutNs;
    friend bool operator==(const State&, const State&) = default;
  };

  // Replaces the timeout before any order is registered. Zero is invalid.
  [[nodiscard]] base::ErrorCode Configure(
      uint64_t response_timeout_ns) noexcept;
  // Stages a new dense order slot; index must equal order_count().
  [[nodiscard]] OrderRequestExecutionTransition PrepareQueueNew(
      size_t index, model::ClientOrderId id) const noexcept;
  // Stages a newer cancel ID after the preceding cancel and result have ended.
  [[nodiscard]] OrderRequestExecutionTransition PrepareQueueCancel(
      size_t index, model::ClientOrderId id,
      model::CancelRequestId cancel_id) const noexcept;
  // Stages the FIFO head as attempted. The host sends only after committing.
  [[nodiscard]] OrderRequestExecutionTransition PrepareBeginSend(
      uint64_t now) const noexcept;
  // Validates request association after the core has accepted a new report.
  // next_order is the candidate business snapshot.
  [[nodiscard]] OrderRequestExecutionTransition PrepareReport(
      size_t index, const model::TradingEvent& event,
      const model::OrderSnapshot& next_order) const noexcept;
  // Stages a queued new request's local shutdown completion, preserving the
  // order core until the resulting business event is consumed.
  [[nodiscard]] OrderRequestExecutionTransition PrepareCancelBeforeSend(
      size_t index, uint64_t now) const noexcept;
  // Stages FIFO local-result consumption alongside its core business event.
  [[nodiscard]] OrderRequestExecutionTransition PrepareConsumeLocalResult()
      const noexcept;
  // Stages a send failure, timeout or disconnect for one attempted request.
  [[nodiscard]] OrderRequestExecutionTransition PrepareUncertain(
      const OrderExecutionRequest& request) const noexcept;
  // Applies a current successful candidate. The host retains its request value
  // if it needs to send it after this call.
  void Commit(const OrderRequestExecutionTransition& transition) noexcept;

  // Returns the next request or local result, or no value for an empty queue.
  std::optional<OrderExecutionRequest> front() const noexcept;
  std::optional<OrderExecutionLocalResult> local_front() const noexcept;
  // Returns the earliest due InFlight request, with stable dense-index ordering
  // when deadlines match. Uncertain and completed requests have no deadline.
  std::optional<OrderExecutionRequest> due(uint64_t now) const noexcept;
  std::optional<uint64_t> next_deadline() const noexcept;
  // Includes queued, InFlight, Uncertain and unconsumed local results.
  bool is_drained() const noexcept;
  size_t order_count() const noexcept { return state_.order_count; }
  size_t request_count() const noexcept { return state_.request_count; }
  size_t local_count() const noexcept { return state_.local_count; }
  const State& state() const noexcept { return state_; }

 private:
  // Creates a one-order candidate carrying the current mutation generation.
  OrderRequestExecutionTransition Prepare(size_t index) const noexcept;
  // Locates a live queue entry; missing membership indicates corrupt state.
  std::optional<size_t> FindQueued(
      const OrderExecutionRequest& request) const noexcept;
  // Removes a known ring entry while preserving FIFO order and usable capacity.
  void RemoveQueued(const OrderExecutionRequest& request) noexcept;

  State state_;
  uint64_t generation_ = 0;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_ORDER_REQUEST_EXECUTOR_H_
