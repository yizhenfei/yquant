#include "yquant/trading/mock_trading_adapter.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <chrono>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {
namespace {

constexpr size_t kNoIndex = std::numeric_limits<size_t>::max();
constexpr size_t kPlanCapacity = 2 * kMaxTradingOrders;
constexpr size_t kLookupCapacity = 2 * kMaxTradingOrders;
static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);

// Writes a bounded, deterministic session-local ID, retaining no input views.
// A zero ordinal identifies an order; positive ordinals identify its fills.
template <typename Id>
base::ErrorCode MakeAdapterId(const char* prefix, uint64_t order_id,
                              uint32_t ordinal, Id& out) noexcept {
  char text[65];
  const int size = ordinal == 0
                       ? ::yquant::base::FormatTo(text, sizeof(text), "{}{}",
                                                  prefix, order_id)
                       : ::yquant::base::FormatTo(text, sizeof(text), "{}{}-{}",
                                                  prefix, order_id, ordinal);
  if (size < 0 || static_cast<size_t>(size) >= sizeof(text)) {
    return base::ErrorCode::kInternalError;
  }
  return out.Assign({text, static_cast<size_t>(size)});
}

// Converts UTC without overflowing a signed nanosecond intermediate. Failure
// leaves out unchanged; monotonic receive time is recorded separately.
base::ErrorCode ReadUtcNanoseconds(uint64_t& out) noexcept {
  const auto duration = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto fraction =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds)
          .count();
  if (seconds.count() < 0 || fraction < 0) {
    return base::ErrorCode::kUnsupported;
  }
  constexpr uint64_t kNanosecondsPerSecond = 1000000000;
  const auto whole = static_cast<uint64_t>(seconds.count());
  const auto remainder = static_cast<uint64_t>(fraction);
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
    model::AdapterOrderId broker_id;
    model::OrderRequest request;
    model::OrderState state = model::OrderState::kPendingNew;
    int64_t cumulative_qty = 0;
    uint64_t report_sequence = 0;
    uint32_t fill_count = 0;
    model::CancelRequestId cancel_id;
    model::CancelRequestId last_cancel_id;
    Step step = Step::kNone;
    size_t step_position = kNoIndex;
    size_t cancel_position = kNoIndex;
  };

  struct Plan {
    size_t order_index = 0;
    PlanKind kind = PlanKind::kOrderStep;
    uint64_t deadline = 0;
    uint64_t registration = 0;
  };

  TradingConfig config;
  std::array<OrderSlot, kMaxTradingOrders> orders;
  // Zero marks an empty hash bucket; other values hold order index plus one.
  std::array<size_t, kLookupCapacity> lookup = {};
  std::array<Plan, kPlanCapacity> plans;
  size_t order_count = 0;
  size_t active_orders = 0;
  size_t plan_count = 0;
  uint64_t last_registration = 0;

  // Returns the matching bucket or an empty bucket without growing storage.
  size_t FindBucket(model::ClientOrderId id) const noexcept {
    auto hash = id.value;
    hash = (hash ^ (hash >> 30)) * 0xbf58476d1ce4e5b9ULL;
    hash = (hash ^ (hash >> 27)) * 0x94d049bb133111ebULL;
    size_t bucket = (hash ^ (hash >> 31)) & (kLookupCapacity - 1);
    for (size_t checked = 0; checked < kLookupCapacity; ++checked) {
      if (lookup[bucket] == 0 || orders[lookup[bucket] - 1].id == id) {
        return bucket;
      }
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
  void SetPosition(const Plan& plan, size_t position) noexcept {
    auto& order = orders[plan.order_index];
    if (plan.kind == PlanKind::kOrderStep) {
      order.step_position = position;
    } else {
      order.cancel_position = position;
    }
  }

  void SwapPlans(size_t first, size_t second) noexcept {
    std::swap(plans[first], plans[second]);
    SetPosition(plans[first], first);
    SetPosition(plans[second], second);
  }

  // Restores ordering upward; each iteration moves one level toward the root.
  void SiftUp(size_t position) noexcept {
    while (position != 0) {
      const auto parent = (position - 1) / 2;
      if (!Earlier(plans[position], plans[parent])) {
        break;
      }
      SwapPlans(position, parent);
      position = parent;
    }
  }

  // Restores ordering downward in at most the height of the fixed heap.
  void SiftDown(size_t position) noexcept {
    while (position * 2 + 1 < plan_count) {
      auto child = position * 2 + 1;
      if (child + 1 < plan_count && Earlier(plans[child + 1], plans[child])) {
        ++child;
      }
      if (!Earlier(plans[child], plans[position])) {
        break;
      }
      SwapPlans(position, child);
      position = child;
    }
  }

  // Caller has checked capacity and serial overflow before committing state.
  void AddPlan(size_t order_index, PlanKind kind, uint64_t deadline) noexcept {
    const auto position = plan_count++;
    plans[position] = {order_index, kind, deadline, ++last_registration};
    SetPosition(plans[position], position);
    SiftUp(position);
  }

  // Removes an existing handle, including a future fill canceled by another
  // event. Repair both heap directions after replacing an arbitrary element.
  void RemovePlan(size_t position) noexcept {
    SetPosition(plans[position], kNoIndex);
    --plan_count;
    if (position == plan_count) {
      return;
    }
    plans[position] = plans[plan_count];
    SetPosition(plans[position], position);
    if (position != 0 && Earlier(plans[position], plans[(position - 1) / 2])) {
      SiftUp(position);
    } else {
      SiftDown(position);
    }
  }
};

// Stages a scheduled report before heap mutation.
struct MockTradingAdapter::PreparedReport {
  Storage::Plan plan;
  model::TradingEvent event;
  Storage::Step next_step = Storage::Step::kNone;
  bool becomes_terminal = false;
};

MockTradingAdapter::MockTradingAdapter() noexcept = default;
MockTradingAdapter::~MockTradingAdapter() noexcept = default;

base::Status MockTradingAdapter::Error(base::ErrorCode code,
                                       const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
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
  base::ConfigError error;
  const auto status = ValidateTradingConfig(config, error);
  if (!status.ok()) {
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}: {}",
                             error.field_path, error.message);
    return status;
  }
  std::unique_ptr<Storage> storage(new (std::nothrow) Storage{});
  if (!storage) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "cannot allocate mock counter storage");
  }
  storage->config = config;
  storage_ = std::move(storage);
  state_ = State::kPrepared;
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::Start(uint64_t) noexcept {
  if (state_ != State::kPrepared) {
    return Error(base::ErrorCode::kInvalidState,
                 "counter must be prepared before activation");
  }
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
                                           uint64_t now) noexcept {
  if (state_ != State::kActive) {
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  }
  if (id.value == 0 || request.instrument_id.value == 0 ||
      (request.side != model::OrderSide::kBuy &&
       request.side != model::OrderSide::kSell) ||
      request.limit_price <= 0 || request.quantity <= 0) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "invalid limit order request");
  }
  auto& storage = *storage_;
  const auto bucket = storage.FindBucket(id);
  if (bucket == kNoIndex) {
    return Error(base::ErrorCode::kCorrupt, "mock order lookup is full");
  }
  if (storage.lookup[bucket] != 0) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "client order ID was already sent");
  }
  if (storage.order_count == kMaxTradingOrders ||
      storage.plan_count == kPlanCapacity ||
      storage.last_registration == UINT64_MAX) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "mock counter capacity or plan sequence exhausted");
  }
  if (now > UINT64_MAX - storage.config.report_interval_ns) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "order acknowledgement deadline overflows");
  }
  Storage::OrderSlot order;
  const auto id_status =
      MakeAdapterId("mock-order-", id.value, 0, order.broker_id);
  if (id_status != base::ErrorCode::kOk) {
    return Error(id_status, "cannot encode mock broker order ID");
  }
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
    model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
    model::CancelRequestId cancel_request_id, uint64_t now) noexcept {
  if (state_ != State::kActive) {
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  }
  if (id.value == 0 || cancel_request_id.value == 0 ||
      adapter_order_id.empty()) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "invalid cancel request identity");
  }
  auto& storage = *storage_;
  const auto bucket = storage.FindBucket(id);
  if (bucket == kNoIndex || storage.lookup[bucket] == 0) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "cancel refers to an unknown order");
  }
  const auto index = storage.lookup[bucket] - 1;
  auto& order = storage.orders[index];
  if (order.broker_id != adapter_order_id ||
      cancel_request_id.value <= order.last_cancel_id.value) {
    return Error(base::ErrorCode::kInvalidArgument,
                 "cancel broker or attempt identity does not match");
  }
  if (order.state == model::OrderState::kPendingNew ||
      order.cancel_position != kNoIndex) {
    return Error(base::ErrorCode::kInvalidState,
                 "order is pending acceptance or another cancel");
  }
  if (storage.plan_count == kPlanCapacity ||
      storage.last_registration == UINT64_MAX) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "cancel plan capacity exhausted");
  }
  if (now > UINT64_MAX - storage.config.report_interval_ns) {
    return Error(base::ErrorCode::kResourceExhausted,
                 "cancel deadline overflows");
  }
  order.cancel_id = cancel_request_id;
  order.last_cancel_id = cancel_request_id;
  storage.AddPlan(index, Storage::PlanKind::kCancel,
                  now + storage.config.report_interval_ns);
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::PrepareScheduledReport(
    uint64_t now, PreparedReport& report) noexcept {
  auto& storage = *storage_;
  const auto& plan = report.plan;
  const auto& order = storage.orders[plan.order_index];
  auto& event = report.event;
  event.client_order_id = order.id;
  event.adapter_order_id = order.broker_id;
  event.report_sequence = order.report_sequence + 1;
  event.receive_time_ns = now;
  event.cumulative_filled_qty = order.cumulative_qty;
  auto& next_step = report.next_step;
  auto& becomes_terminal = report.becomes_terminal;
  switch (plan.kind) {
    case Storage::PlanKind::kCancel:
      event.cancel_request_id = order.cancel_id;
      if (order.state == model::OrderState::kWorking) {
        event.kind = model::OrderUpdateKind::kCanceled;
        becomes_terminal = true;
      } else {
        event.kind = model::OrderUpdateKind::kCancelRejected;
        event.reason.Set(2, "mock order has no working remainder");
      }
      return {};
    case Storage::PlanKind::kOrderStep:
      break;
  }
  switch (order.step) {
    case Storage::Step::kAccept:
      event.kind = model::OrderUpdateKind::kAccepted;
      if (storage.config.mode == MockTradingMode::kFill) {
        next_step = order.request.quantity > 1 ? Storage::Step::kFirstFill
                                               : Storage::Step::kFinalFill;
      }
      return {};
    case Storage::Step::kReject:
      event.kind = model::OrderUpdateKind::kRejected;
      event.adapter_order_id = {};
      event.reason.Set(1, "mock counter rejected the order");
      becomes_terminal = true;
      return {};
    case Storage::Step::kFirstFill:
    case Storage::Step::kFinalFill:
      return PrepareFillReport(now, report);
    case Storage::Step::kNone:
      return PollError(base::ErrorCode::kCorrupt,
                       "mock order has no scheduled step")
          .status;
  }
  return PollError(base::ErrorCode::kCorrupt,
                   "mock order has no scheduled step")
      .status;
}

base::Status MockTradingAdapter::PrepareFillReport(
    uint64_t now, PreparedReport& report) noexcept {
  const auto& order = storage_->orders[report.plan.order_index];
  auto& event = report.event;
  auto& next_step = report.next_step;
  auto& becomes_terminal = report.becomes_terminal;
  event.kind = model::OrderUpdateKind::kFill;
  const auto quantity = order.step == Storage::Step::kFirstFill
                            ? order.request.quantity / 2
                            : order.request.quantity - order.cumulative_qty;
  if (quantity <= 0 ||
      quantity > order.request.quantity - order.cumulative_qty) {
    return PollError(base::ErrorCode::kCorrupt, "invalid mock fill quantity")
        .status;
  }
  auto& fill = event.fill;
  fill.client_order_id = order.id;
  fill.adapter_order_id = order.broker_id;
  fill.instrument_id = order.request.instrument_id;
  fill.side = order.request.side;
  fill.price = order.request.limit_price;
  fill.quantity = quantity;
  fill.receive_time_ns = now;
  auto code = MakeAdapterId("mock-fill-", order.id.value, order.fill_count + 1,
                            fill.execution_id);
  if (code != base::ErrorCode::kOk) {
    return PollError(code, "cannot encode mock execution ID").status;
  }
  code = ReadUtcNanoseconds(fill.event_time_ns);
  if (code != base::ErrorCode::kOk) {
    return PollError(code, "cannot represent mock execution UTC time").status;
  }
  fill.has_event_time = true;
  event.cumulative_filled_qty += quantity;
  becomes_terminal = event.cumulative_filled_qty == order.request.quantity;
  if (!becomes_terminal) {
    next_step = Storage::Step::kFinalFill;
  }
  return {};
}

void MockTradingAdapter::CommitScheduledReport(
    uint64_t now, const PreparedReport& report) noexcept {
  auto& storage = *storage_;
  const auto& plan = report.plan;
  auto& order = storage.orders[plan.order_index];
  const auto& event = report.event;
  const auto next_step = report.next_step;
  const auto becomes_terminal = report.becomes_terminal;
  // All possible failures precede this commit. Indexed removal also cancels
  // future fills without leaving stale heap nodes for later Poll calls.
  storage.RemovePlan(0);
  ++order.report_sequence;
  if (event.kind == model::OrderUpdateKind::kAccepted) {
    order.state = model::OrderState::kWorking;
  } else if (event.kind == model::OrderUpdateKind::kRejected) {
    order.state = model::OrderState::kRejected;
  } else if (event.kind == model::OrderUpdateKind::kFill) {
    order.cumulative_qty = event.cumulative_filled_qty;
    ++order.fill_count;
    order.state = becomes_terminal ? model::OrderState::kFilled
                                   : model::OrderState::kWorking;
  } else if (event.kind == model::OrderUpdateKind::kCanceled) {
    order.state = model::OrderState::kCanceled;
    if (order.step_position != kNoIndex) {
      storage.RemovePlan(order.step_position);
    }
    order.step = Storage::Step::kNone;
  }
  if (becomes_terminal) {
    --storage.active_orders;
  }
  if (plan.kind == Storage::PlanKind::kCancel) {
    order.cancel_id = {};
  } else {
    order.step = next_step;
  }
  if (next_step != Storage::Step::kNone) {
    storage.AddPlan(plan.order_index, Storage::PlanKind::kOrderStep,
                    now + storage.config.report_interval_ns);
  }
}

TradingPollResult MockTradingAdapter::Poll(uint64_t now,
                                           model::TradingEvent& out) noexcept {
  if (state_ != State::kActive) {
    return {TradingPollState::kError,
            Error(base::ErrorCode::kInvalidState, "counter is not active")};
  }
  auto& storage = *storage_;
  if (storage.plan_count == 0 || storage.plans[0].deadline > now) {
    return {};
  }
  const auto& order = storage.orders[storage.plans[0].order_index];
  if (order.report_sequence == UINT64_MAX) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "order report sequence exhausted");
  }

  PreparedReport report;
  report.plan = storage.plans[0];
  const auto prepared = PrepareScheduledReport(now, report);
  if (!prepared.ok()) {
    return {TradingPollState::kError, prepared};
  }
  const auto next_step = report.next_step;
  const auto becomes_terminal = report.becomes_terminal;
  const auto& event = report.event;
  if (next_step != Storage::Step::kNone &&
      (now > UINT64_MAX - storage.config.report_interval_ns ||
       storage.last_registration == UINT64_MAX)) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "next mock order step overflows");
  }
  if (becomes_terminal && storage.active_orders == 0) {
    return PollError(base::ErrorCode::kCorrupt,
                     "mock active-order count is inconsistent");
  }
  CommitScheduledReport(now, report);
  out = event;
  return {TradingPollState::kEvent, {}};
}

std::optional<uint64_t> MockTradingAdapter::next_deadline() const noexcept {
  if (state_ != State::kActive || storage_->plan_count == 0) {
    return {};
  }
  return storage_->plans[0].deadline;
}

bool MockTradingAdapter::is_drained() const noexcept {
  return storage_ ? storage_->active_orders == 0 && storage_->plan_count == 0
                  : drained_on_stop_;
}

base::Status MockTradingAdapter::Stop() noexcept {
  if (state_ == State::kStopped) {
    return {};
  }
  drained_on_stop_ = is_drained();
  storage_.reset();
  state_ = State::kStopped;
  return {};
}

void MockTradingAdapter::Reset() noexcept {
  (void)Stop();
  state_ = State::kUnprepared;
  drained_on_stop_ = true;
  error_detail_[0] = '\0';
}

}  // namespace yquant::trading
