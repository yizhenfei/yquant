#include "yquant/trading/order_request_executor.h"

#include <assert.h>

#include <array>
#include <optional>

#include "yquant/base/status.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;

// Keeps dense-order scans stable, checking a new request before its cancel.
constexpr std::array kRequestKinds{OrderRequestKind::kNew,
                                   OrderRequestKind::kCancel};

// Returns an unsuccessful candidate without any executable queue changes.
OrderRequestExecutionTransition Rejected(ErrorCode code,
                                         const char* detail) noexcept {
  OrderRequestExecutionTransition result;
  result.code = code;
  result.detail = detail;
  return result;
}

// Builds an identity from its retained dense-order record.
OrderExecutionRequest RequestFor(size_t index,
                                 const RequestExecution& execution,
                                 OrderRequestKind kind) noexcept {
  OrderExecutionRequest request;
  request.kind = kind;
  request.order_index = index;
  request.client_order_id = execution.client_order_id;
  switch (kind) {
    case OrderRequestKind::kNew:
      break;
    case OrderRequestKind::kCancel:
      request.cancel_request_id = execution.cancel_request_id;
      break;
  }
  return request;
}

// Returns the progress matching a request kind.
OrderRequestProgress& Progress(RequestExecution& execution,
                               OrderRequestKind kind) noexcept {
  switch (kind) {
    case OrderRequestKind::kNew:
      return execution.new_request;
    case OrderRequestKind::kCancel:
      return execution.cancel_request;
  }
  assert(false);
  return execution.new_request;
}

// Uses the same request-kind dispatch for read-only scheduling.
const OrderRequestProgress& Progress(const RequestExecution& execution,
                                     OrderRequestKind kind) noexcept {
  switch (kind) {
    case OrderRequestKind::kNew:
      return execution.new_request;
    case OrderRequestKind::kCancel:
      return execution.cancel_request;
  }
  assert(false);
  return execution.new_request;
}

// Identifies which retained request kinds are present for this order.
bool HasRequest(const RequestExecution& execution,
                OrderRequestKind kind) noexcept {
  switch (kind) {
    case OrderRequestKind::kNew:
      return execution.client_order_id.value != 0;
    case OrderRequestKind::kCancel:
      return execution.cancel_request_id.value != 0;
  }
  return false;
}

// Checks that an identity names a present retained request.
bool Matches(const OrderExecutionRequest& request,
             const RequestExecution& execution) noexcept {
  if (request.client_order_id != execution.client_order_id) {
    return false;
  }
  switch (request.kind) {
    case OrderRequestKind::kNew:
      return request.cancel_request_id.value == 0;
    case OrderRequestKind::kCancel:
      return request.cancel_request_id.value != 0 &&
             request.cancel_request_id == execution.cancel_request_id;
  }
  return false;
}

// Completes a request without losing whether an external effect was possible.
void Complete(OrderRequestProgress& progress) noexcept {
  progress.state = OrderRequestState::kCompleted;
  progress.response_deadline_ns = 0;
}

}  // namespace

ErrorCode OrderRequestExecutor::Configure(
    uint64_t response_timeout_ns) noexcept {
  if (response_timeout_ns == 0) {
    return ErrorCode::kInvalidArgument;
  }
  if (state_.order_count != 0) {
    return ErrorCode::kInvalidState;
  }
  state_.response_timeout_ns = response_timeout_ns;
  ++generation_;
  return ErrorCode::kOk;
}

OrderRequestExecutionTransition OrderRequestExecutor::Prepare(
    size_t index) const noexcept {
  if (index >= state_.order_count) {
    return Rejected(ErrorCode::kInvalidArgument, "unknown request order slot");
  }
  OrderRequestExecutionTransition result;
  result.order_index = index;
  result.next = state_.executions[index];
  result.generation = generation_;
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareQueueNew(
    size_t index, model::ClientOrderId id) const noexcept {
  if (index != state_.order_count || id.value == 0 ||
      (index != 0 &&
       id.value <= state_.executions[index - 1].client_order_id.value)) {
    return Rejected(ErrorCode::kInvalidArgument,
                    "new request requires a new dense slot and increasing ID");
  }
  if (index == kMaxTradingOrders ||
      state_.request_count == kMaxTradingRequests) {
    return Rejected(ErrorCode::kResourceExhausted,
                    "request or order tracking capacity exhausted");
  }
  OrderRequestExecutionTransition result;
  result.order_index = index;
  result.next.client_order_id = id;
  result.next.new_request.state = OrderRequestState::kQueued;
  result.enqueue_request =
      RequestFor(index, result.next, OrderRequestKind::kNew);
  result.register_order = true;
  result.generation = generation_;
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareQueueCancel(
    size_t index, model::ClientOrderId id,
    model::CancelRequestId cancel_id) const noexcept {
  auto result = Prepare(index);
  if (!result.ok()) {
    return result;
  }
  auto& execution = result.next;
  if (id != execution.client_order_id || cancel_id.value == 0 ||
      cancel_id.value <= execution.cancel_request_id.value) {
    return Rejected(ErrorCode::kInvalidArgument,
                    "cancel request identity mismatch");
  }
  if (execution.new_request.state != OrderRequestState::kCompleted ||
      !execution.new_request.attempted || execution.new_local_result_pending ||
      execution.cancel_request.state != OrderRequestState::kCompleted ||
      execution.cancel_local_result_pending) {
    return Rejected(ErrorCode::kInvalidState,
                    "another request is still unresolved");
  }
  if (state_.request_count == kMaxTradingRequests) {
    return Rejected(ErrorCode::kResourceExhausted, "request queue is full");
  }
  execution.cancel_request_id = cancel_id;
  execution.cancel_request = {};
  execution.cancel_request.state = OrderRequestState::kQueued;
  result.enqueue_request =
      RequestFor(index, execution, OrderRequestKind::kCancel);
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareBeginSend(
    uint64_t now) const noexcept {
  const auto request = front();
  if (!request) {
    return Rejected(ErrorCode::kInvalidState, "no request is queued");
  }
  auto result = Prepare(request->order_index);
  auto& progress = Progress(result.next, request->kind);
  if (!Matches(*request, result.next) ||
      progress.state != OrderRequestState::kQueued || progress.attempted) {
    return Rejected(ErrorCode::kCorrupt, "queued request tracking disagrees");
  }
  if (now > UINT64_MAX - state_.response_timeout_ns) {
    return Rejected(ErrorCode::kInvalidArgument, "response deadline overflows");
  }
  progress.state = OrderRequestState::kInFlight;
  progress.attempted = true;
  progress.attempted_at_ns = now;
  progress.response_deadline_ns = now + state_.response_timeout_ns;
  result.remove_request = request;
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareReport(
    size_t index, const model::TradingEvent& event,
    const model::OrderSnapshot& next_order) const noexcept {
  auto result = Prepare(index);
  if (!result.ok()) {
    return result;
  }
  auto& execution = result.next;
  if (event.client_order_id != execution.client_order_id ||
      next_order.client_order_id != execution.client_order_id ||
      !execution.new_request.attempted || execution.new_local_result_pending) {
    return Rejected(ErrorCode::kCorrupt,
                    "broker report precedes an attempted new request");
  }
  switch (event.kind) {
    case model::OrderUpdateKind::kAccepted:
    case model::OrderUpdateKind::kRejected:
    case model::OrderUpdateKind::kFill:
      if (event.cancel_request_id.value != 0) {
        return Rejected(ErrorCode::kCorrupt,
                        "unexpected report cancel identity");
      }
      break;
    case model::OrderUpdateKind::kCanceled:
      if (event.cancel_request_id.value == 0) {
        break;
      }
      [[fallthrough]];
    case model::OrderUpdateKind::kCancelRejected:
      if (event.cancel_request_id.value == 0 ||
          event.cancel_request_id != execution.cancel_request_id ||
          !execution.cancel_request.attempted ||
          execution.cancel_request.state == OrderRequestState::kQueued ||
          execution.cancel_request.state == OrderRequestState::kCompleted) {
        return Rejected(ErrorCode::kCorrupt,
                        "cancel response does not match an attempted request");
      }
      Complete(execution.cancel_request);
      break;
    case model::OrderUpdateKind::kCanceledBeforeSend:
    case model::OrderUpdateKind::kCancelNotSent:
      return Rejected(ErrorCode::kCorrupt,
                      "local result received as broker report");
  }
  if (static_cast<uint8_t>(event.kind) >
      static_cast<uint8_t>(model::OrderUpdateKind::kCancelNotSent)) {
    return Rejected(ErrorCode::kCorrupt, "unknown broker report kind");
  }
  Complete(execution.new_request);
  if (event.kind == model::OrderUpdateKind::kFill &&
      next_order.state == model::OrderState::kFilled &&
      execution.cancel_request_id.value != 0 &&
      execution.cancel_request.state == OrderRequestState::kQueued) {
    if (state_.local_count == kMaxLocalOrderUpdates) {
      return Rejected(ErrorCode::kResourceExhausted,
                      "local result queue is full");
    }
    const auto request =
        RequestFor(index, execution, OrderRequestKind::kCancel);
    if (!FindQueued(request)) {
      return Rejected(ErrorCode::kCorrupt,
                      "queued cancel has no send arrangement");
    }
    Complete(execution.cancel_request);
    execution.cancel_local_result_pending = true;
    result.remove_request = request;
    result.append_result = OrderExecutionLocalResult{
        OrderLocalResultKind::kCancelNotSent, request, event.receive_time_ns};
  }
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareCancelBeforeSend(
    size_t index, uint64_t now) const noexcept {
  auto result = Prepare(index);
  if (!result.ok()) {
    return result;
  }
  auto& execution = result.next;
  if (execution.new_request.state != OrderRequestState::kQueued ||
      execution.new_request.attempted || execution.new_local_result_pending) {
    return Rejected(ErrorCode::kInvalidState,
                    "new request is no longer unsent");
  }
  if (state_.local_count == kMaxLocalOrderUpdates) {
    return Rejected(ErrorCode::kResourceExhausted,
                    "local result queue is full");
  }
  const auto request = RequestFor(index, execution, OrderRequestKind::kNew);
  if (!FindQueued(request)) {
    return Rejected(ErrorCode::kCorrupt, "queued new has no send arrangement");
  }
  Complete(execution.new_request);
  execution.new_local_result_pending = true;
  result.remove_request = request;
  result.append_result = OrderExecutionLocalResult{
      OrderLocalResultKind::kCanceledBeforeSend, request, now};
  return result;
}

OrderRequestExecutionTransition
OrderRequestExecutor::PrepareConsumeLocalResult() const noexcept {
  const auto local = local_front();
  if (!local) {
    return Rejected(ErrorCode::kInvalidState, "no local result is queued");
  }
  auto result = Prepare(local->request.order_index);
  if (!result.ok()) {
    return result;
  }
  if (!Matches(local->request, result.next)) {
    return Rejected(ErrorCode::kCorrupt,
                    "local result request identity mismatch");
  }
  switch (local->kind) {
    case OrderLocalResultKind::kCanceledBeforeSend:
      if (!result.next.new_local_result_pending) {
        return Rejected(ErrorCode::kCorrupt, "new local result is not pending");
      }
      result.next.new_local_result_pending = false;
      break;
    case OrderLocalResultKind::kCancelNotSent:
      if (!result.next.cancel_local_result_pending) {
        return Rejected(ErrorCode::kCorrupt,
                        "cancel local result is not pending");
      }
      result.next.cancel_local_result_pending = false;
      break;
  }
  result.consume_result = true;
  return result;
}

OrderRequestExecutionTransition OrderRequestExecutor::PrepareUncertain(
    const OrderExecutionRequest& request) const noexcept {
  auto result = Prepare(request.order_index);
  if (!result.ok()) {
    return result;
  }
  if (!Matches(request, result.next)) {
    return Rejected(ErrorCode::kInvalidArgument,
                    "uncertain request identity mismatch");
  }
  auto& progress = Progress(result.next, request.kind);
  switch (progress.state) {
    case OrderRequestState::kInFlight:
    case OrderRequestState::kUncertain:
      progress.state = OrderRequestState::kUncertain;
      progress.response_deadline_ns = 0;
      return result;
    case OrderRequestState::kQueued:
    case OrderRequestState::kCompleted:
      return Rejected(ErrorCode::kInvalidState,
                      "request has no unresolved attempt");
  }
  return Rejected(ErrorCode::kCorrupt, "unknown request state");
}

std::optional<size_t> OrderRequestExecutor::FindQueued(
    const OrderExecutionRequest& request) const noexcept {
  for (size_t offset = 0; offset < state_.request_count; ++offset) {
    if (state_.requests[(state_.request_head + offset) % kMaxTradingRequests] ==
        request) {
      return offset;
    }
  }
  return std::nullopt;
}

void OrderRequestExecutor::RemoveQueued(
    const OrderExecutionRequest& request) noexcept {
  const auto offset = FindQueued(request);
  assert(offset.has_value());
  if (*offset == 0) {
    state_.request_head = (state_.request_head + 1) % kMaxTradingRequests;
  } else {
    for (size_t next = *offset + 1; next < state_.request_count; ++next) {
      state_.requests[(state_.request_head + next - 1) % kMaxTradingRequests] =
          state_.requests[(state_.request_head + next) % kMaxTradingRequests];
    }
  }
  --state_.request_count;
}

void OrderRequestExecutor::Commit(
    const OrderRequestExecutionTransition& transition) noexcept {
  assert(transition.ok());
  assert(transition.generation == generation_);
  assert(transition.order_index < kMaxTradingOrders);
  if (transition.remove_request) {
    RemoveQueued(*transition.remove_request);
  }
  if (transition.enqueue_request) {
    state_.requests[(state_.request_head + state_.request_count) %
                    kMaxTradingRequests] = *transition.enqueue_request;
    ++state_.request_count;
  }
  if (transition.consume_result) {
    state_.local_head = (state_.local_head + 1) % kMaxLocalOrderUpdates;
    --state_.local_count;
  }
  if (transition.append_result) {
    state_.local_results[(state_.local_head + state_.local_count) %
                         kMaxLocalOrderUpdates] = *transition.append_result;
    ++state_.local_count;
  }
  state_.executions[transition.order_index] = transition.next;
  if (transition.register_order) {
    ++state_.order_count;
  }
  ++generation_;
}

std::optional<OrderExecutionRequest> OrderRequestExecutor::front()
    const noexcept {
  if (state_.request_count == 0) {
    return std::nullopt;
  }
  return state_.requests[state_.request_head];
}

std::optional<OrderExecutionLocalResult> OrderRequestExecutor::local_front()
    const noexcept {
  if (state_.local_count == 0) {
    return std::nullopt;
  }
  return state_.local_results[state_.local_head];
}

std::optional<OrderExecutionRequest> OrderRequestExecutor::due(
    uint64_t now) const noexcept {
  std::optional<OrderExecutionRequest> earliest;
  uint64_t deadline = now;
  for (size_t i = 0; i < state_.order_count; ++i) {
    const auto& execution = state_.executions[i];
    for (const auto kind : kRequestKinds) {
      if (!HasRequest(execution, kind)) {
        continue;
      }
      const auto& progress = Progress(execution, kind);
      if (progress.state == OrderRequestState::kInFlight &&
          progress.response_deadline_ns <= deadline &&
          (!earliest || progress.response_deadline_ns < deadline)) {
        earliest = RequestFor(i, execution, kind);
        deadline = progress.response_deadline_ns;
      }
    }
  }
  return earliest;
}

std::optional<uint64_t> OrderRequestExecutor::next_deadline() const noexcept {
  const auto request = due(UINT64_MAX);
  if (!request) {
    return std::nullopt;
  }
  const auto& execution = state_.executions[request->order_index];
  return Progress(execution, request->kind).response_deadline_ns;
}

bool OrderRequestExecutor::is_drained() const noexcept {
  if (state_.request_count != 0 || state_.local_count != 0) {
    return false;
  }
  for (size_t i = 0; i < state_.order_count; ++i) {
    const auto& execution = state_.executions[i];
    if (execution.new_request.state != OrderRequestState::kCompleted ||
        (execution.cancel_request_id.value != 0 &&
         execution.cancel_request.state != OrderRequestState::kCompleted)) {
      return false;
    }
  }
  return true;
}

}  // namespace yquant::trading
