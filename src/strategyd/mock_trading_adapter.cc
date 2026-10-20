#include "strategyd/mock_trading_adapter.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <new>
#include <utility>

#include "strategyd/trading_limits.h"

namespace yquant::strategyd {
namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kPlanCapacity = 2 * kMaxTradingOrders;
constexpr std::size_t kLookupCapacity = 2 * kMaxTradingOrders;
static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);

// Writes a bounded, deterministic session-local ID, retaining no input views.
// A zero ordinal identifies an order; positive ordinals identify its fills.
template <typename Id>
base::ErrorCode MakeExternalId(const char* prefix, std::uint64_t order_id,
                               std::uint32_t ordinal, Id& out) noexcept {
  char text[65];
  const int size =
      ordinal == 0
          ? std::snprintf(text, sizeof(text), "%s%llu", prefix,
                          static_cast<unsigned long long>(order_id))
          : std::snprintf(text, sizeof(text), "%s%llu-%u", prefix,
                          static_cast<unsigned long long>(order_id), ordinal);
  if (size < 0 || static_cast<std::size_t>(size) >= sizeof(text)) {
    return base::ErrorCode::kInternalError;
  }
  return out.Assign({text, static_cast<std::size_t>(size)});
}

// Converts UTC without overflowing a signed nanosecond intermediate. Failure
// leaves out unchanged; monotonic receive time is recorded separately.
base::ErrorCode ReadUtcNanoseconds(std::uint64_t& out) noexcept {
  const auto duration = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto fraction =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds)
          .count();
  if (seconds.count() < 0 || fraction < 0) return base::ErrorCode::kUnsupported;
  constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
  const auto whole = static_cast<std::uint64_t>(seconds.count());
  const auto remainder = static_cast<std::uint64_t>(fraction);
  if (whole > (UINT64_MAX - remainder) / kNanosecondsPerSecond) {
    return base::ErrorCode::kResourceExhausted;
  }
  out = whole * kNanosecondsPerSecond + remainder;
  return base::ErrorCode::kOk;
}

}  // namespace

// One allocation owns all order records, lookup buckets, and scheduled plans.
// Every order has at most one order-step plan and one cancel plan. The indexed
// heap removes canceled fill plans immediately, so Poll never scans stale work.
struct MockTradingAdapter::Storage {
  enum class PlanKind { kOrderStep, kCancel };
  enum class Step { kAccept, kReject, kFirstFill, kFinalFill, kNone };

  struct OrderSlot {
    model::ClientOrderId id;
    model::BrokerOrderId broker_id;
    model::OrderRequest request;
    model::OrderState state = model::OrderState::kPendingNew;
    std::int64_t cumulative_qty = 0;
    std::uint64_t report_sequence = 0;
    std::uint32_t fill_count = 0;
    model::CancelRequestId cancel_id;
    model::CancelRequestId last_cancel_id;
    Step step = Step::kNone;
    std::size_t step_position = kNoIndex;
    std::size_t cancel_position = kNoIndex;
  };

  struct Plan {
    std::size_t order_index = 0;
    PlanKind kind = PlanKind::kOrderStep;
    std::uint64_t deadline = 0;
    std::uint64_t registration = 0;
  };

  TradingConfig config;
  std::array<OrderSlot, kMaxTradingOrders> orders;
  // Zero marks an empty hash bucket; other values hold order index plus one.
  std::array<std::size_t, kLookupCapacity> lookup = {};
  std::array<Plan, kPlanCapacity> plans;
  std::size_t order_count = 0;
  std::size_t active_orders = 0;
  std::size_t plan_count = 0;
  std::uint64_t last_registration = 0;

  // Returns the matching bucket or an empty bucket without growing storage.
  std::size_t FindBucket(model::ClientOrderId id) const noexcept {
    auto hash = id.value;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    std::size_t bucket = (hash ^ (hash >> 31)) & (kLookupCapacity - 1);
    for (std::size_t checked = 0; checked < kLookupCapacity; ++checked) {
      if (lookup[bucket] == 0 || orders[lookup[bucket] - 1].id == id)
        return bucket;
      bucket = (bucket + 1) & (kLookupCapacity - 1);
    }
    return kNoIndex;
  }

  // Chronological ordering uses registration as a stable tie breaker.
  static bool Earlier(const Plan& left, const Plan& right) noexcept {
    return left.deadline < right.deadline ||
           (left.deadline == right.deadline &&
            left.registration < right.registration);
  }

  // Keeps the owning order's handle synchronized with heap movement/removal.
  void SetPosition(const Plan& plan, std::size_t position) noexcept {
    auto& order = orders[plan.order_index];
    if (plan.kind == PlanKind::kOrderStep)
      order.step_position = position;
    else
      order.cancel_position = position;
  }

  void SwapPlans(std::size_t first, std::size_t second) noexcept {
    std::swap(plans[first], plans[second]);
    SetPosition(plans[first], first);
    SetPosition(plans[second], second);
  }

  // Restores ordering upward; each iteration moves one level toward the root.
  void SiftUp(std::size_t position) noexcept {
    while (position != 0) {
      const auto parent = (position - 1) / 2;
      if (!Earlier(plans[position], plans[parent])) break;
      SwapPlans(position, parent);
      position = parent;
    }
  }

  // Restores ordering downward in at most the height of the fixed heap.
  void SiftDown(std::size_t position) noexcept {
    while (position * 2 + 1 < plan_count) {
      auto child = position * 2 + 1;
      if (child + 1 < plan_count && Earlier(plans[child + 1], plans[child]))
        ++child;
      if (!Earlier(plans[child], plans[position])) break;
      SwapPlans(position, child);
      position = child;
    }
  }

  // Caller has checked capacity and serial overflow before committing state.
  void AddPlan(std::size_t order_index, PlanKind kind,
               std::uint64_t deadline) noexcept {
    const auto position = plan_count++;
    plans[position] = {order_index, kind, deadline, ++last_registration};
    SetPosition(plans[position], position);
    SiftUp(position);
  }

  // Removes an existing handle, including a future fill canceled by another
  // event. Repair both heap directions after replacing an arbitrary element.
  void RemovePlan(std::size_t position) noexcept {
    SetPosition(plans[position], kNoIndex);
    --plan_count;
    if (position == plan_count) return;
    plans[position] = plans[plan_count];
    SetPosition(plans[position], position);
    if (position != 0 && Earlier(plans[position], plans[(position - 1) / 2]))
      SiftUp(position);
    else
      SiftDown(position);
  }
};

MockTradingAdapter::MockTradingAdapter() noexcept = default;
MockTradingAdapter::~MockTradingAdapter() noexcept = default;

base::Status MockTradingAdapter::Error(base::ErrorCode code,
                                       const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

TradingPollResult MockTradingAdapter::PollError(base::ErrorCode code,
                                                const char* detail) noexcept {
  state_ = State::kFailed;
  return {TradingPollState::kError, Error(code, detail)};
}

base::Status MockTradingAdapter::Prepare(const TradingConfig& config) noexcept {
  if (state_ != State::kUnprepared) {
    return Error(base::ErrorCode::kInvalidState,
                 "counter can only be prepared once");
  }
  strategy::ConfigError error;
  const auto status = ValidateTradingConfig(config, error);
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return status;
  }
  std::unique_ptr<Storage> storage(new (std::nothrow) Storage{});
  if (!storage)
    return Error(base::ErrorCode::kResourceExhausted,
                 "cannot allocate mock counter storage");
  storage->config = config;
  storage_ = std::move(storage);
  state_ = State::kPrepared;
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::Start(std::uint64_t) noexcept {
  if (state_ != State::kPrepared)
    return Error(base::ErrorCode::kInvalidState,
                 "counter must be prepared before activation");
  state_ = State::kActive;
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::ReadAccountSnapshot(
    TradingAccountSnapshot& out) noexcept {
  if (state_ != State::kActive || storage_->order_count != 0) {
    return Error(base::ErrorCode::kInvalidState,
                 "initial account requires an active session without orders");
  }
  out = storage_->config.initial_account;
  return {};
}

base::Status MockTradingAdapter::SendOrder(model::ClientOrderId id,
                                           const model::OrderRequest& request,
                                           std::uint64_t now) noexcept {
  if (state_ != State::kActive)
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  if (id.value == 0 || request.instrument_id.value == 0 ||
      (request.side != model::OrderSide::kBuy &&
       request.side != model::OrderSide::kSell) ||
      request.limit_price <= 0 || request.quantity <= 0) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "invalid limit order request");
  }
  auto& storage = *storage_;
  const auto bucket = storage.FindBucket(id);
  if (bucket == kNoIndex)
    return Error(base::ErrorCode::kCorrupt, "mock order lookup is full");
  if (storage.lookup[bucket] != 0)
    return Error(base::ErrorCode::kInvalidArgument,
                 "client order ID was already sent");
  if (storage.order_count == kMaxTradingOrders ||
      storage.plan_count == kPlanCapacity ||
      storage.last_registration == UINT64_MAX) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "mock counter capacity or plan sequence exhausted");
  }
  if (now > UINT64_MAX - storage.config.report_interval_ns)
    return Error(base::ErrorCode::kResourceExhausted,
                 "order acknowledgement deadline overflows");
  Storage::OrderSlot order;
  const auto id_status =
      MakeExternalId("mock-order-", id.value, 0, order.broker_id);
  if (id_status != base::ErrorCode::kOk)
    return Error(id_status, "cannot encode mock broker order ID");
  order.id = id;
  order.request = request;
  order.step = storage.config.mode == MockTradingMode::kReject
                   ? Storage::Step::kReject
                   : Storage::Step::kAccept;
  const auto index = storage.order_count;
  storage.orders[index] = order;
  storage.lookup[bucket] = index + 1;
  ++storage.order_count;
  ++storage.active_orders;
  storage.AddPlan(index, Storage::PlanKind::kOrderStep,
                  now + storage.config.report_interval_ns);
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::SendCancel(
    model::ClientOrderId id, const model::BrokerOrderId& broker_order_id,
    model::CancelRequestId cancel_request_id, std::uint64_t now) noexcept {
  if (state_ != State::kActive)
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  if (id.value == 0 || cancel_request_id.value == 0 || broker_order_id.empty())
    return Error(base::ErrorCode::kInvalidArgument,
                 "invalid cancel request identity");
  auto& storage = *storage_;
  const auto bucket = storage.FindBucket(id);
  if (bucket == kNoIndex || storage.lookup[bucket] == 0)
    return Error(base::ErrorCode::kInvalidArgument,
                 "cancel refers to an unknown order");
  const auto index = storage.lookup[bucket] - 1;
  auto& order = storage.orders[index];
  if (order.broker_id != broker_order_id ||
      cancel_request_id.value <= order.last_cancel_id.value) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "cancel broker or attempt identity does not match");
  }
  if (order.state == model::OrderState::kPendingNew ||
      order.cancel_position != kNoIndex)
    return Error(base::ErrorCode::kInvalidState,
                 "order is pending acceptance or another cancel");
  if (storage.plan_count == kPlanCapacity ||
      storage.last_registration == UINT64_MAX)
    return Error(base::ErrorCode::kResourceExhausted,
                 "cancel plan capacity exhausted");
  if (now > UINT64_MAX - storage.config.report_interval_ns)
    return Error(base::ErrorCode::kResourceExhausted,
                 "cancel deadline overflows");
  order.cancel_id = cancel_request_id;
  order.last_cancel_id = cancel_request_id;
  storage.AddPlan(index, Storage::PlanKind::kCancel,
                  now + storage.config.report_interval_ns);
  error_detail_[0] = '\0';
  return {};
}

TradingPollResult MockTradingAdapter::Poll(std::uint64_t now,
                                           TradingEvent& out) noexcept {
  if (state_ != State::kActive)
    return {TradingPollState::kError,
            Error(base::ErrorCode::kInvalidState, "counter is not active")};
  auto& storage = *storage_;
  if (storage.plan_count == 0 || storage.plans[0].deadline > now) return {};
  const auto plan = storage.plans[0];
  auto& order = storage.orders[plan.order_index];
  if (order.report_sequence == UINT64_MAX)
    return PollError(base::ErrorCode::kResourceExhausted,
                     "order report sequence exhausted");

  TradingEvent event;
  event.client_order_id = order.id;
  event.broker_order_id = order.broker_id;
  event.report_sequence = order.report_sequence + 1;
  event.receive_time_ns = now;
  event.cumulative_filled_qty = order.cumulative_qty;
  auto next_step = Storage::Step::kNone;
  bool becomes_terminal = false;
  if (plan.kind == Storage::PlanKind::kCancel) {
    event.cancel_request_id = order.cancel_id;
    if (order.state == model::OrderState::kWorking) {
      event.kind = model::OrderUpdateKind::kCanceled;
      becomes_terminal = true;
    } else {
      event.kind = model::OrderUpdateKind::kCancelRejected;
      event.reason.Set(2, "mock order has no working remainder");
    }
  } else if (order.step == Storage::Step::kAccept) {
    event.kind = model::OrderUpdateKind::kAccepted;
    if (storage.config.mode == MockTradingMode::kFill) {
      next_step = order.request.quantity > 1 ? Storage::Step::kFirstFill
                                             : Storage::Step::kFinalFill;
    }
  } else if (order.step == Storage::Step::kReject) {
    event.kind = model::OrderUpdateKind::kRejected;
    event.broker_order_id = {};
    event.reason.Set(1, "mock counter rejected the order");
    becomes_terminal = true;
  } else if (order.step == Storage::Step::kFirstFill ||
             order.step == Storage::Step::kFinalFill) {
    event.kind = model::OrderUpdateKind::kFill;
    const auto quantity = order.step == Storage::Step::kFirstFill
                              ? order.request.quantity / 2
                              : order.request.quantity - order.cumulative_qty;
    if (quantity <= 0 ||
        quantity > order.request.quantity - order.cumulative_qty)
      return PollError(base::ErrorCode::kCorrupt, "invalid mock fill quantity");
    auto& fill = event.fill;
    fill.client_order_id = order.id;
    fill.broker_order_id = order.broker_id;
    fill.instrument_id = order.request.instrument_id;
    fill.side = order.request.side;
    fill.price = order.request.limit_price;
    fill.quantity = quantity;
    fill.receive_time_ns = now;
    auto code = MakeExternalId("mock-fill-", order.id.value,
                               order.fill_count + 1, fill.execution_id);
    if (code != base::ErrorCode::kOk)
      return PollError(code, "cannot encode mock execution ID");
    code = ReadUtcNanoseconds(fill.event_time_ns);
    if (code != base::ErrorCode::kOk)
      return PollError(code, "cannot represent mock execution UTC time");
    fill.has_event_time = true;
    event.cumulative_filled_qty += quantity;
    becomes_terminal = event.cumulative_filled_qty == order.request.quantity;
    if (!becomes_terminal) next_step = Storage::Step::kFinalFill;
  } else {
    return PollError(base::ErrorCode::kCorrupt,
                     "mock order has no scheduled step");
  }
  if (next_step != Storage::Step::kNone &&
      (now > UINT64_MAX - storage.config.report_interval_ns ||
       storage.last_registration == UINT64_MAX)) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "next mock order step overflows");
  }
  if (becomes_terminal && storage.active_orders == 0)
    return PollError(base::ErrorCode::kCorrupt,
                     "mock active-order count is inconsistent");

  // All possible failures precede this commit. Indexed removal also cancels
  // future fills without leaving stale heap nodes for later Poll calls.
  storage.RemovePlan(0);
  ++order.report_sequence;
  if (event.kind == model::OrderUpdateKind::kAccepted)
    order.state = model::OrderState::kWorking;
  else if (event.kind == model::OrderUpdateKind::kRejected)
    order.state = model::OrderState::kRejected;
  else if (event.kind == model::OrderUpdateKind::kFill) {
    order.cumulative_qty = event.cumulative_filled_qty;
    ++order.fill_count;
    order.state = becomes_terminal ? model::OrderState::kFilled
                                   : model::OrderState::kWorking;
  } else if (event.kind == model::OrderUpdateKind::kCanceled) {
    order.state = model::OrderState::kCanceled;
    if (order.step_position != kNoIndex)
      storage.RemovePlan(order.step_position);
    order.step = Storage::Step::kNone;
  }
  if (becomes_terminal) --storage.active_orders;
  if (plan.kind == Storage::PlanKind::kCancel)
    order.cancel_id = {};
  else
    order.step = next_step;
  if (next_step != Storage::Step::kNone)
    storage.AddPlan(plan.order_index, Storage::PlanKind::kOrderStep,
                    now + storage.config.report_interval_ns);
  out = event;
  return {TradingPollState::kEvent, {}};
}

std::optional<std::uint64_t> MockTradingAdapter::next_deadline()
    const noexcept {
  if (state_ != State::kActive || storage_->plan_count == 0) return {};
  return storage_->plans[0].deadline;
}

bool MockTradingAdapter::is_drained() const noexcept {
  return storage_ ? storage_->active_orders == 0 && storage_->plan_count == 0
                  : drained_on_stop_;
}

base::Status MockTradingAdapter::Stop() noexcept {
  if (state_ == State::kStopped) return {};
  drained_on_stop_ = is_drained();
  storage_.reset();
  state_ = State::kStopped;
  return {};
}

}  // namespace yquant::strategyd
