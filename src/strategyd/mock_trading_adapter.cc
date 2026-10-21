#include "strategyd/mock_trading_adapter.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "strategyd/trading_limits.h"

namespace yquant::strategyd {
namespace {

constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
constexpr std::size_t kPlanCapacity = 2 * kMaxTradingOrders;
constexpr std::size_t kLookupCapacity = 2 * kMaxTradingOrders;
constexpr std::uint32_t kStateMagic = 0x534d5159;     // YQMS
constexpr std::uint32_t kMutationMagic = 0x4d4d5159;  // YQMM
constexpr std::uint32_t kCodecVersion = 1;
constexpr std::size_t kChecksumBytes = 8;
static_assert((kLookupCapacity & (kLookupCapacity - 1)) == 0);

// Explicit byte codec shared by snapshots and per-operation journal entries.
// A failed reader/writer stays failed and never accesses beyond its span.
class Writer {
 public:
  explicit Writer(std::span<std::byte> output) noexcept : output_(output) {}
  void U8(std::uint8_t value) noexcept { Integer(value, 1); }
  void U32(std::uint32_t value) noexcept { Integer(value, 4); }
  void U64(std::uint64_t value) noexcept { Integer(value, 8); }
  void I64(std::int64_t value) noexcept {
    U64(std::bit_cast<std::uint64_t>(value));
  }
  void Bytes(std::string_view value) noexcept {
    if (!ok_ || value.size() > output_.size() - size_) {
      ok_ = false;
      return;
    }
    if (!value.empty())
      std::memcpy(output_.data() + size_, value.data(), value.size());
    size_ += value.size();
  }
  template <class Id>
  void IdBytes(const Id& id) noexcept {
    U8(static_cast<std::uint8_t>(id.view().size()));
    Bytes(id.view());
  }
  bool ok() const noexcept { return ok_; }
  std::size_t size() const noexcept { return size_; }

 private:
  void Integer(std::uint64_t value, std::size_t size) noexcept {
    if (!ok_ || size > output_.size() - size_) {
      ok_ = false;
      return;
    }
    for (std::size_t index = 0; index < size; ++index) {
      output_[size_++] = static_cast<std::byte>(value & 0xffU);
      value >>= 8;
    }
  }
  std::span<std::byte> output_;
  std::size_t size_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> input) noexcept : input_(input) {}
  std::uint8_t U8() noexcept { return static_cast<std::uint8_t>(Integer(1)); }
  std::uint32_t U32() noexcept {
    return static_cast<std::uint32_t>(Integer(4));
  }
  std::uint64_t U64() noexcept { return Integer(8); }
  std::int64_t I64() noexcept { return std::bit_cast<std::int64_t>(U64()); }
  bool Bool() noexcept {
    const auto value = U8();
    if (value > 1) ok_ = false;
    return value != 0;
  }
  std::string_view SizedBytes(std::size_t maximum) noexcept {
    const auto size = U8();
    if (size > maximum) {
      ok_ = false;
      return {};
    }
    return Bytes(size);
  }
  template <class Id>
  void IdBytes(Id& id) noexcept {
    const auto size = U8();
    const auto value = Bytes(size);
    if (!ok_) return;
    id = {};
    if (size != 0 && id.Assign(value) != base::ErrorCode::kOk) ok_ = false;
  }
  std::string_view Bytes(std::size_t size) noexcept {
    if (!ok_ || size > input_.size() - offset_) {
      ok_ = false;
      return {};
    }
    const std::string_view result(
        reinterpret_cast<const char*>(input_.data() + offset_), size);
    offset_ += size;
    return result;
  }
  bool ok() const noexcept { return ok_; }
  bool done() const noexcept { return ok_ && offset_ == input_.size(); }

 private:
  std::uint64_t Integer(std::size_t size) noexcept {
    if (!ok_ || size > input_.size() - offset_) {
      ok_ = false;
      return 0;
    }
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < size; ++index) {
      value |= std::to_integer<std::uint64_t>(input_[offset_++]) << (8 * index);
    }
    return value;
  }
  std::span<const std::byte> input_;
  std::size_t offset_ = 0;
  bool ok_ = true;
};

// The outer WAL also protects entries; this checksum detects standalone
// snapshot damage before a candidate is considered for installation.
std::uint64_t Checksum(std::span<const std::byte> input) noexcept {
  std::uint64_t value = 14695981039346656037ULL;
  for (const auto byte : input) {
    value ^= std::to_integer<std::uint8_t>(byte);
    value *= 1099511628211ULL;
  }
  return value;
}

bool CheckedPayload(std::span<const std::byte> input,
                    std::span<const std::byte>& payload) noexcept {
  if (input.size() < kChecksumBytes) return false;
  const auto prefix = input.first(input.size() - kChecksumBytes);
  Reader checksum(input.last(kChecksumBytes));
  if (checksum.U64() != Checksum(prefix)) return false;
  payload = prefix;
  return true;
}

void PutRequest(Writer& writer, const model::OrderRequest& request) noexcept {
  writer.U64(request.instrument_id.value);
  writer.U8(static_cast<std::uint8_t>(request.side));
  writer.I64(request.limit_price);
  writer.I64(request.quantity);
}

model::OrderRequest GetRequest(Reader& reader) noexcept {
  model::OrderRequest request;
  request.instrument_id.value = reader.U64();
  request.side = static_cast<model::OrderSide>(reader.U8());
  request.limit_price = reader.I64();
  request.quantity = reader.I64();
  return request;
}

bool ValidRequest(const model::OrderRequest& request) noexcept {
  return request.instrument_id.value != 0 &&
         (request.side == model::OrderSide::kBuy ||
          request.side == model::OrderSide::kSell) &&
         request.limit_price > 0 && request.quantity > 0;
}

void PutFill(Writer& writer, const model::Fill& fill) noexcept {
  writer.U64(fill.client_order_id.value);
  writer.IdBytes(fill.broker_order_id);
  writer.IdBytes(fill.execution_id);
  writer.U64(fill.instrument_id.value);
  writer.U8(static_cast<std::uint8_t>(fill.side));
  writer.I64(fill.price);
  writer.I64(fill.quantity);
  writer.U64(fill.event_time_ns);
  writer.U64(fill.receive_time_ns);
  writer.U8(fill.has_event_time ? 1 : 0);
}

model::Fill GetFill(Reader& reader) noexcept {
  model::Fill fill;
  fill.client_order_id.value = reader.U64();
  reader.IdBytes(fill.broker_order_id);
  reader.IdBytes(fill.execution_id);
  fill.instrument_id.value = reader.U64();
  fill.side = static_cast<model::OrderSide>(reader.U8());
  fill.price = reader.I64();
  fill.quantity = reader.I64();
  fill.event_time_ns = reader.U64();
  fill.receive_time_ns = reader.U64();
  fill.has_event_time = reader.Bool();
  return fill;
}

void PutEvent(Writer& writer, const TradingEvent& event) noexcept {
  writer.U64(event.client_order_id.value);
  writer.IdBytes(event.broker_order_id);
  writer.U64(event.report_sequence);
  writer.U8(static_cast<std::uint8_t>(event.kind));
  writer.I64(event.cumulative_filled_qty);
  writer.U64(event.receive_time_ns);
  writer.U64(event.cancel_request_id.value);
  writer.U32(std::bit_cast<std::uint32_t>(event.reason.code));
  writer.U8(event.reason.truncated ? 1 : 0);
  writer.U8(event.reason.size);
  writer.Bytes(event.reason.view());
  PutFill(writer, event.fill);
}

TradingEvent GetEvent(Reader& reader) noexcept {
  TradingEvent event;
  event.client_order_id.value = reader.U64();
  reader.IdBytes(event.broker_order_id);
  event.report_sequence = reader.U64();
  event.kind = static_cast<model::OrderUpdateKind>(reader.U8());
  event.cumulative_filled_qty = reader.I64();
  event.receive_time_ns = reader.U64();
  event.cancel_request_id.value = reader.U64();
  const auto code = std::bit_cast<std::int32_t>(reader.U32());
  const bool truncated = reader.Bool();
  const auto text = reader.SizedBytes(sizeof(event.reason.text) - 1);
  event.reason.Set(code, text);
  event.reason.truncated = event.reason.truncated || truncated;
  event.fill = GetFill(reader);
  return event;
}

bool SameFill(const model::Fill& left, const model::Fill& right) noexcept {
  return left.client_order_id == right.client_order_id &&
         left.broker_order_id == right.broker_order_id &&
         left.execution_id == right.execution_id &&
         left.instrument_id == right.instrument_id && left.side == right.side &&
         left.price == right.price && left.quantity == right.quantity &&
         left.event_time_ns == right.event_time_ns &&
         left.receive_time_ns == right.receive_time_ns &&
         left.has_event_time == right.has_event_time;
}

bool SameEvent(const TradingEvent& left, const TradingEvent& right) noexcept {
  return left.client_order_id == right.client_order_id &&
         left.broker_order_id == right.broker_order_id &&
         left.report_sequence == right.report_sequence &&
         left.kind == right.kind &&
         left.cumulative_filled_qty == right.cumulative_filled_qty &&
         left.receive_time_ns == right.receive_time_ns &&
         left.cancel_request_id == right.cancel_request_id &&
         left.reason == right.reason && SameFill(left.fill, right.fill);
}

bool ValidEvent(const TradingEvent& event) noexcept {
  if (event.client_order_id.value == 0 || event.report_sequence == 0 ||
      event.kind > model::OrderUpdateKind::kCancelRejected ||
      event.cumulative_filled_qty < 0 || event.reason.size > 128)
    return false;
  if (event.kind == model::OrderUpdateKind::kFill) {
    const auto& fill = event.fill;
    return fill.client_order_id == event.client_order_id &&
           fill.broker_order_id == event.broker_order_id &&
           !fill.broker_order_id.empty() && !fill.execution_id.empty() &&
           fill.instrument_id.value != 0 &&
           (fill.side == model::OrderSide::kBuy ||
            fill.side == model::OrderSide::kSell) &&
           fill.price > 0 && fill.quantity > 0 &&
           fill.quantity <= event.cumulative_filled_qty &&
           fill.receive_time_ns == event.receive_time_ns && fill.has_event_time;
  }
  return SameFill(event.fill, model::Fill{});
}

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

base::Status EncodeMockTradingMutation(const MockTradingMutation& mutation,
                                       std::span<std::byte> output,
                                       std::size_t& used) noexcept {
  if ((mutation.kind == MockTradingMutationKind::kSendOrder &&
       (mutation.client_order_id.value == 0 ||
        !ValidRequest(mutation.request))) ||
      (mutation.kind == MockTradingMutationKind::kSendCancel &&
       (mutation.client_order_id.value == 0 ||
        mutation.broker_order_id.empty() ||
        mutation.cancel_request_id.value == 0)) ||
      (mutation.kind == MockTradingMutationKind::kReport &&
       (!ValidEvent(mutation.event) ||
        mutation.event.receive_time_ns != mutation.now)) ||
      mutation.kind < MockTradingMutationKind::kSendOrder ||
      mutation.kind > MockTradingMutationKind::kReport) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  Writer writer(output);
  writer.U32(kMutationMagic);
  writer.U32(kCodecVersion);
  writer.U8(static_cast<std::uint8_t>(mutation.kind));
  writer.U64(mutation.now);
  if (mutation.kind == MockTradingMutationKind::kSendOrder) {
    writer.U64(mutation.client_order_id.value);
    PutRequest(writer, mutation.request);
    writer.IdBytes(mutation.request_key);
  } else if (mutation.kind == MockTradingMutationKind::kSendCancel) {
    writer.U64(mutation.client_order_id.value);
    writer.IdBytes(mutation.broker_order_id);
    writer.U64(mutation.cancel_request_id.value);
  } else {
    PutEvent(writer, mutation.event);
  }
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  writer.U64(Checksum(output.first(writer.size())));
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  used = writer.size();
  return {};
}

base::Status DecodeMockTradingMutation(std::span<const std::byte> input,
                                       MockTradingMutation& out) noexcept {
  std::span<const std::byte> payload;
  if (!CheckedPayload(input, payload)) return {base::ErrorCode::kCorrupt, {}};
  Reader reader(payload);
  if (reader.U32() != kMutationMagic || reader.U32() != kCodecVersion)
    return {base::ErrorCode::kCorrupt, {}};
  MockTradingMutation mutation;
  mutation.kind = static_cast<MockTradingMutationKind>(reader.U8());
  mutation.now = reader.U64();
  if (mutation.kind == MockTradingMutationKind::kSendOrder) {
    mutation.client_order_id.value = reader.U64();
    mutation.request = GetRequest(reader);
    reader.IdBytes(mutation.request_key);
  } else if (mutation.kind == MockTradingMutationKind::kSendCancel) {
    mutation.client_order_id.value = reader.U64();
    reader.IdBytes(mutation.broker_order_id);
    mutation.cancel_request_id.value = reader.U64();
  } else if (mutation.kind == MockTradingMutationKind::kReport) {
    mutation.event = GetEvent(reader);
  } else {
    return {base::ErrorCode::kCorrupt, {}};
  }
  // Reuse the encoder's semantic validation with a bounded stack buffer.
  std::array<std::byte, kMaxMockTradingMutationBytes> validation;
  std::size_t size = 0;
  if (!reader.done() ||
      !EncodeMockTradingMutation(mutation, validation, size).ok()) {
    return {base::ErrorCode::kCorrupt, {}};
  }
  out = mutation;
  return {};
}

// One allocation owns all order records, lookup buckets, and scheduled plans.
// Every order has at most one order-step plan and one cancel plan. The indexed
// heap removes canceled fill plans immediately, so Poll never scans stale work.
struct MockTradingAdapter::Storage {
  enum class PlanKind { kOrderStep, kCancel };
  enum class Step { kAccept, kReject, kFirstFill, kFinalFill, kNone };

  struct OrderSlot {
    model::ClientOrderId id;
    model::BrokerOrderId broker_id;
    model::BrokerOrderId request_key;
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
  std::array<std::size_t, kLookupCapacity> identity_lookup = {};
  std::array<Plan, kPlanCapacity> plans;
  std::array<TradingEvent, kMaxMockTradingReports> events;
  // Fill indices avoid retaining a second copy of each execution.
  std::array<std::size_t, kMaxMockTradingReports> fill_events;
  std::size_t order_count = 0;
  std::size_t active_orders = 0;
  std::size_t plan_count = 0;
  std::uint64_t last_registration = 0;
  std::size_t event_count = 0;
  std::size_t fill_count = 0;

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

  // External keys use a separate bounded index; empty keys are never indexed.
  std::size_t FindIdentityBucket(std::string_view key) const noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : key) {
      hash ^= static_cast<unsigned char>(byte);
      hash *= 1099511628211ULL;
    }
    auto bucket = static_cast<std::size_t>(hash) & (kLookupCapacity - 1);
    for (std::size_t checked = 0; checked < kLookupCapacity; ++checked) {
      const auto slot = identity_lookup[bucket];
      if (slot == 0 || orders[slot - 1].request_key.view() == key)
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
  return SendOrderWithIdentity(id, request, {}, now);
}

void MockTradingAdapter::SetMutationObserver(
    MockTradingMutationObserver observer, void* context) noexcept {
  observer_ = observer;
  observer_context_ = context;
}

base::Status MockTradingAdapter::Observe(
    const MockTradingMutation& mutation) noexcept {
  if (!observer_) return {};
  const auto status = observer_(observer_context_, mutation);
  if (!status.ok())
    static_cast<void>(
        Error(status.code, "mock mutation observer rejected journal entry"));
  return status;
}

base::Status MockTradingAdapter::SendOrderWithIdentity(
    model::ClientOrderId id, const model::OrderRequest& request,
    std::string_view request_key, std::uint64_t now) noexcept {
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
  auto identity_bucket = kNoIndex;
  if (!request_key.empty()) {
    if (order.request_key.Assign(request_key) != base::ErrorCode::kOk)
      return Error(base::ErrorCode::kInvalidArgument,
                   "external request key exceeds mock identity capacity");
    identity_bucket = storage.FindIdentityBucket(request_key);
    if (identity_bucket == kNoIndex ||
        storage.identity_lookup[identity_bucket] != 0)
      return Error(base::ErrorCode::kInvalidArgument,
                   "external request key was already sent");
  }
  order.step = storage.config.mode == MockTradingMode::kReject
                   ? Storage::Step::kReject
                   : Storage::Step::kAccept;
  MockTradingMutation mutation;
  mutation.kind = MockTradingMutationKind::kSendOrder;
  mutation.now = now;
  mutation.client_order_id = id;
  mutation.request = request;
  mutation.request_key = order.request_key;
  const auto observed = Observe(mutation);
  if (!observed.ok()) return observed;
  const auto index = storage.order_count;
  storage.orders[index] = order;
  storage.lookup[bucket] = index + 1;
  if (identity_bucket != kNoIndex)
    storage.identity_lookup[identity_bucket] = index + 1;
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
  MockTradingMutation mutation;
  mutation.kind = MockTradingMutationKind::kSendCancel;
  mutation.now = now;
  mutation.client_order_id = id;
  mutation.broker_order_id = broker_order_id;
  mutation.cancel_request_id = cancel_request_id;
  const auto observed = Observe(mutation);
  if (!observed.ok()) return observed;
  order.cancel_id = cancel_request_id;
  order.last_cancel_id = cancel_request_id;
  storage.AddPlan(index, Storage::PlanKind::kCancel,
                  now + storage.config.report_interval_ns);
  error_detail_[0] = '\0';
  return {};
}

TradingPollResult MockTradingAdapter::Poll(std::uint64_t now,
                                           TradingEvent& out) noexcept {
  return PollImpl(now, out, nullptr);
}

TradingPollResult MockTradingAdapter::PollImpl(
    std::uint64_t now, TradingEvent& out, const TradingEvent* replay) noexcept {
  if (state_ != State::kActive)
    return {TradingPollState::kError,
            Error(base::ErrorCode::kInvalidState, "counter is not active")};
  auto& storage = *storage_;
  if (storage.plan_count == 0 || storage.plans[0].deadline > now) return {};
  const auto plan = storage.plans[0];
  auto& order = storage.orders[plan.order_index];
  if (storage.event_count == kMaxMockTradingReports)
    return PollError(base::ErrorCode::kResourceExhausted,
                     "mock report history capacity exhausted");
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
    if (replay) {
      fill.event_time_ns = replay->fill.event_time_ns;
      code = base::ErrorCode::kOk;
    } else {
      code = ReadUtcNanoseconds(fill.event_time_ns);
    }
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
  if (replay && !SameEvent(event, *replay))
    return {TradingPollState::kError,
            Error(base::ErrorCode::kCorrupt,
                  "recorded mock report does not match scheduled plan")};
  if (!replay) {
    MockTradingMutation mutation;
    mutation.kind = MockTradingMutationKind::kReport;
    mutation.now = now;
    mutation.event = event;
    const auto observed = Observe(mutation);
    if (!observed.ok()) return {TradingPollState::kError, observed};
  }

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
  if (event.kind == model::OrderUpdateKind::kFill)
    storage.fill_events[storage.fill_count++] = storage.event_count;
  storage.events[storage.event_count++] = event;
  out = event;
  return {TradingPollState::kEvent, {}};
}

base::Status MockTradingAdapter::ReplayMutation(
    const MockTradingMutation& mutation) noexcept {
  if (state_ != State::kActive)
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  std::array<std::byte, kMaxMockTradingMutationBytes> validation;
  std::size_t used = 0;
  if (!EncodeMockTradingMutation(mutation, validation, used).ok())
    return Error(base::ErrorCode::kCorrupt, "invalid mock replay mutation");
  const auto observer = observer_;
  const auto context = observer_context_;
  const auto state = state_;
  observer_ = nullptr;
  observer_context_ = nullptr;
  base::Status status;
  if (mutation.kind == MockTradingMutationKind::kSendOrder) {
    status = SendOrderWithIdentity(mutation.client_order_id, mutation.request,
                                   mutation.request_key.view(), mutation.now);
  } else if (mutation.kind == MockTradingMutationKind::kSendCancel) {
    status = SendCancel(mutation.client_order_id, mutation.broker_order_id,
                        mutation.cancel_request_id, mutation.now);
  } else {
    TradingEvent event;
    const auto result = PollImpl(mutation.now, event, &mutation.event);
    status = result.state == TradingPollState::kEvent
                 ? base::Status{}
                 : Error(base::ErrorCode::kCorrupt,
                         "mock replay has no matching due report");
  }
  observer_ = observer;
  observer_context_ = context;
  if (!status.ok()) state_ = state;
  return status;
}

base::Status MockTradingAdapter::EncodeState(std::span<std::byte> output,
                                             std::size_t& used,
                                             std::uint64_t now) const noexcept {
  if (state_ != State::kActive || !storage_)
    return {base::ErrorCode::kInvalidState, {}};
  const auto& storage = *storage_;
  Writer writer(output);
  writer.U32(kStateMagic);
  writer.U32(kCodecVersion);
  writer.U64(now);
  writer.U8(static_cast<std::uint8_t>(storage.config.mode));
  writer.U64(storage.config.report_interval_ns);
  writer.U64(storage.config.shutdown_timeout_ns);
  const auto& account = storage.config.initial_account;
  writer.I64(account.cash_balance);
  writer.U32(static_cast<std::uint32_t>(account.position_count));
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    writer.U64(position.instrument_id.value);
    writer.I64(position.total_qty);
    writer.I64(position.available_qty);
    writer.I64(position.reserved_qty);
  }
  writer.U32(static_cast<std::uint32_t>(storage.order_count));
  writer.U32(static_cast<std::uint32_t>(storage.plan_count));
  writer.U64(storage.last_registration);
  writer.U32(static_cast<std::uint32_t>(storage.event_count));
  for (std::size_t index = 0; index < storage.order_count; ++index) {
    const auto& order = storage.orders[index];
    writer.U64(order.id.value);
    writer.IdBytes(order.broker_id);
    writer.IdBytes(order.request_key);
    PutRequest(writer, order.request);
    writer.U8(static_cast<std::uint8_t>(order.state));
    writer.I64(order.cumulative_qty);
    writer.U64(order.report_sequence);
    writer.U32(order.fill_count);
    writer.U64(order.cancel_id.value);
    writer.U64(order.last_cancel_id.value);
    writer.U8(static_cast<std::uint8_t>(order.step));
  }
  for (std::size_t index = 0; index < storage.plan_count; ++index) {
    const auto& plan = storage.plans[index];
    writer.U32(static_cast<std::uint32_t>(plan.order_index));
    writer.U8(static_cast<std::uint8_t>(plan.kind));
    writer.U64(plan.deadline > now ? plan.deadline - now : 0);
    writer.U64(plan.registration);
  }
  for (std::size_t index = 0; index < storage.event_count; ++index)
    PutEvent(writer, storage.events[index]);
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  writer.U64(Checksum(output.first(writer.size())));
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  used = writer.size();
  return {};
}

base::Status MockTradingAdapter::RestoreState(
    std::span<const std::byte> input, std::uint64_t now,
    std::uint64_t* saved_now) noexcept {
  if ((state_ != State::kPrepared && state_ != State::kActive) || !storage_)
    return Error(base::ErrorCode::kInvalidState,
                 "mock restore requires a prepared counter");
  std::span<const std::byte> payload;
  if (input.size() > kMaxMockTradingStateBytes ||
      !CheckedPayload(input, payload))
    return Error(base::ErrorCode::kCorrupt, "invalid mock snapshot checksum");
  Reader reader(payload);
  if (reader.U32() != kStateMagic || reader.U32() != kCodecVersion)
    return Error(base::ErrorCode::kCorrupt, "unsupported mock snapshot format");
  const auto capture_now = reader.U64();
  std::unique_ptr<Storage> candidate(new (std::nothrow) Storage{});
  if (!candidate)
    return Error(base::ErrorCode::kResourceExhausted,
                 "cannot allocate mock restore candidate");
  candidate->config = storage_->config;
  auto& config = candidate->config;
  config.mode = static_cast<MockTradingMode>(reader.U8());
  config.report_interval_ns = reader.U64();
  config.shutdown_timeout_ns = reader.U64();
  auto& account = config.initial_account;
  account = {};
  account.cash_balance = reader.I64();
  account.position_count = reader.U32();
  if (account.position_count > account.positions.size())
    return Error(base::ErrorCode::kCorrupt, "mock account exceeds capacity");
  for (std::size_t index = 0; index < account.position_count; ++index) {
    auto& position = account.positions[index];
    position.instrument_id.value = reader.U64();
    position.total_qty = reader.I64();
    position.available_qty = reader.I64();
    position.reserved_qty = reader.I64();
  }
  strategy::ConfigError config_error;
  if (!reader.ok() || !ValidateTradingConfig(config, config_error).ok() ||
      config.mode != storage_->config.mode ||
      config.report_interval_ns != storage_->config.report_interval_ns ||
      config.shutdown_timeout_ns != storage_->config.shutdown_timeout_ns)
    return Error(base::ErrorCode::kCorrupt,
                 "mock snapshot configuration does not match");
  candidate->order_count = reader.U32();
  candidate->plan_count = reader.U32();
  candidate->last_registration = reader.U64();
  candidate->event_count = reader.U32();
  if (candidate->order_count > kMaxTradingOrders ||
      candidate->plan_count > kPlanCapacity ||
      candidate->event_count > kMaxMockTradingReports)
    return Error(base::ErrorCode::kCorrupt, "mock snapshot exceeds capacity");
  for (std::size_t index = 0; index < candidate->order_count; ++index) {
    auto& order = candidate->orders[index];
    order.id.value = reader.U64();
    reader.IdBytes(order.broker_id);
    reader.IdBytes(order.request_key);
    order.request = GetRequest(reader);
    order.state = static_cast<model::OrderState>(reader.U8());
    order.cumulative_qty = reader.I64();
    order.report_sequence = reader.U64();
    order.fill_count = reader.U32();
    order.cancel_id.value = reader.U64();
    order.last_cancel_id.value = reader.U64();
    order.step = static_cast<Storage::Step>(reader.U8());
    model::BrokerOrderId expected_broker;
    if (!reader.ok() || order.id.value == 0 || !ValidRequest(order.request) ||
        order.state > model::OrderState::kRejected ||
        order.step > Storage::Step::kNone || order.cumulative_qty < 0 ||
        order.cumulative_qty > order.request.quantity || order.fill_count > 2 ||
        order.cancel_id.value > order.last_cancel_id.value ||
        MakeExternalId("mock-order-", order.id.value, 0, expected_broker) !=
            base::ErrorCode::kOk ||
        expected_broker != order.broker_id)
      return Error(base::ErrorCode::kCorrupt, "invalid mock order snapshot");
    const auto bucket = candidate->FindBucket(order.id);
    if (bucket == kNoIndex || candidate->lookup[bucket] != 0)
      return Error(base::ErrorCode::kCorrupt, "duplicate mock order identity");
    candidate->lookup[bucket] = index + 1;
    if (!order.request_key.empty()) {
      const auto identity_bucket =
          candidate->FindIdentityBucket(order.request_key.view());
      if (identity_bucket == kNoIndex ||
          candidate->identity_lookup[identity_bucket] != 0)
        return Error(base::ErrorCode::kCorrupt,
                     "duplicate mock external request key");
      candidate->identity_lookup[identity_bucket] = index + 1;
    }
    if (!model::IsTerminalOrderState(order.state)) ++candidate->active_orders;
  }
  for (std::size_t index = 0; index < candidate->plan_count; ++index) {
    auto& plan = candidate->plans[index];
    plan.order_index = reader.U32();
    plan.kind = static_cast<Storage::PlanKind>(reader.U8());
    const auto remaining = reader.U64();
    plan.registration = reader.U64();
    if (!reader.ok() || plan.order_index >= candidate->order_count ||
        plan.kind > Storage::PlanKind::kCancel ||
        remaining > UINT64_MAX - now || plan.registration == 0 ||
        plan.registration > candidate->last_registration)
      return Error(base::ErrorCode::kCorrupt, "invalid mock scheduled plan");
    plan.deadline = now + remaining;
    auto& order = candidate->orders[plan.order_index];
    auto& position = plan.kind == Storage::PlanKind::kCancel
                         ? order.cancel_position
                         : order.step_position;
    if (position != kNoIndex ||
        (plan.kind == Storage::PlanKind::kOrderStep &&
         order.step == Storage::Step::kNone) ||
        (plan.kind == Storage::PlanKind::kCancel && order.cancel_id.value == 0))
      return Error(base::ErrorCode::kCorrupt, "duplicate mock scheduled plan");
    position = index;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (candidate->plans[previous].registration == plan.registration)
        return Error(base::ErrorCode::kCorrupt,
                     "duplicate mock plan registration");
    }
  }
  // Validate the complete emitted prefix independently from latest order
  // fields, so an installed checkpoint cannot silently duplicate a fill.
  std::array<std::uint64_t, kMaxTradingOrders> sequences = {};
  std::array<std::int64_t, kMaxTradingOrders> cumulative = {};
  std::array<std::uint32_t, kMaxTradingOrders> fills = {};
  std::array<model::OrderState, kMaxTradingOrders> states = {};
  for (std::size_t index = 0; index < candidate->event_count; ++index) {
    const auto event = GetEvent(reader);
    const auto bucket = candidate->FindBucket(event.client_order_id);
    if (!reader.ok() || !ValidEvent(event) || bucket == kNoIndex ||
        candidate->lookup[bucket] == 0)
      return Error(base::ErrorCode::kCorrupt, "invalid mock report history");
    const auto order_index = candidate->lookup[bucket] - 1;
    const auto& order = candidate->orders[order_index];
    if (event.report_sequence != sequences[order_index] + 1 ||
        (event.kind == model::OrderUpdateKind::kRejected
             ? !event.broker_order_id.empty()
             : event.broker_order_id != order.broker_id))
      return Error(base::ErrorCode::kCorrupt,
                   "mock report sequence or broker identity is inconsistent");
    auto& state = states[order_index];
    if (event.kind == model::OrderUpdateKind::kAccepted) {
      if (state != model::OrderState::kPendingNew)
        return Error(base::ErrorCode::kCorrupt, "duplicate mock acceptance");
      state = model::OrderState::kWorking;
    } else if (event.kind == model::OrderUpdateKind::kRejected) {
      if (state != model::OrderState::kPendingNew)
        return Error(base::ErrorCode::kCorrupt, "invalid mock rejection");
      state = model::OrderState::kRejected;
    } else if (event.kind == model::OrderUpdateKind::kFill) {
      const auto& fill = event.fill;
      model::ExecutionId expected_execution;
      if (state != model::OrderState::kWorking ||
          fill.instrument_id != order.request.instrument_id ||
          fill.side != order.request.side ||
          fill.price != order.request.limit_price ||
          fill.quantity > order.request.quantity - cumulative[order_index] ||
          MakeExternalId("mock-fill-", order.id.value, fills[order_index] + 1,
                         expected_execution) != base::ErrorCode::kOk ||
          fill.execution_id != expected_execution)
        return Error(base::ErrorCode::kCorrupt, "invalid mock fill history");
      cumulative[order_index] += fill.quantity;
      ++fills[order_index];
      if (cumulative[order_index] == order.request.quantity)
        state = model::OrderState::kFilled;
      candidate->fill_events[candidate->fill_count++] = index;
    } else if (event.kind == model::OrderUpdateKind::kCanceled) {
      if (state != model::OrderState::kWorking ||
          event.cancel_request_id.value == 0 ||
          event.cancel_request_id.value > order.last_cancel_id.value)
        return Error(base::ErrorCode::kCorrupt, "invalid mock cancel history");
      state = model::OrderState::kCanceled;
    } else if (event.kind == model::OrderUpdateKind::kCancelRejected) {
      if (!model::IsTerminalOrderState(state) ||
          event.cancel_request_id.value == 0 ||
          event.cancel_request_id.value > order.last_cancel_id.value)
        return Error(base::ErrorCode::kCorrupt,
                     "invalid mock cancel-rejection history");
    }
    if (event.cumulative_filled_qty != cumulative[order_index])
      return Error(base::ErrorCode::kCorrupt,
                   "mock cumulative fill history is inconsistent");
    sequences[order_index] = event.report_sequence;
    candidate->events[index] = event;
  }
  if (!reader.done())
    return Error(base::ErrorCode::kCorrupt, "truncated mock snapshot");
  for (std::size_t index = 0; index < candidate->order_count; ++index) {
    const auto& order = candidate->orders[index];
    if (order.report_sequence != sequences[index] ||
        order.cumulative_qty != cumulative[index] ||
        order.fill_count != fills[index] || order.state != states[index] ||
        (order.cancel_position == kNoIndex) != (order.cancel_id.value == 0) ||
        (order.step_position == kNoIndex) !=
            (order.step == Storage::Step::kNone))
      return Error(base::ErrorCode::kCorrupt,
                   "mock latest order and history disagree");
    Storage::Step expected_step = Storage::Step::kNone;
    if (order.state == model::OrderState::kPendingNew) {
      expected_step = config.mode == MockTradingMode::kReject
                          ? Storage::Step::kReject
                          : Storage::Step::kAccept;
    } else if (order.state == model::OrderState::kWorking &&
               config.mode == MockTradingMode::kFill) {
      expected_step = order.cumulative_qty == 0 && order.request.quantity > 1
                          ? Storage::Step::kFirstFill
                          : Storage::Step::kFinalFill;
    }
    if (order.step != expected_step)
      return Error(base::ErrorCode::kCorrupt, "invalid next mock order step");
  }
  // Overdue deadlines collapse to now and can change registration tie order.
  // Rebuild the candidate heap and its handles after clock normalization.
  for (std::size_t index = candidate->plan_count / 2; index != 0; --index)
    candidate->SiftDown(index - 1);
  storage_ = std::move(candidate);
  state_ = State::kActive;
  drained_on_stop_ = false;
  if (saved_now) *saved_now = capture_now;
  error_detail_[0] = '\0';
  return {};
}

base::Status MockTradingAdapter::RebaseDeadlines(
    std::uint64_t old_now, std::uint64_t new_now) noexcept {
  if (state_ != State::kActive || !storage_)
    return Error(base::ErrorCode::kInvalidState, "counter is not active");
  for (std::size_t index = 0; index < storage_->plan_count; ++index) {
    const auto deadline = storage_->plans[index].deadline;
    const auto remaining = deadline > old_now ? deadline - old_now : 0;
    if (remaining > UINT64_MAX - new_now)
      return Error(base::ErrorCode::kResourceExhausted,
                   "rebased mock deadline overflows");
  }
  for (std::size_t index = 0; index < storage_->plan_count; ++index) {
    auto& deadline = storage_->plans[index].deadline;
    deadline = new_now + (deadline > old_now ? deadline - old_now : 0);
  }
  // Clamping multiple overdue deadlines can change their tie ordering.
  for (std::size_t index = storage_->plan_count / 2; index != 0; --index)
    storage_->SiftDown(index - 1);
  return {};
}

std::size_t MockTradingAdapter::recovery_order_count() const noexcept {
  return storage_ ? storage_->order_count : 0;
}

std::size_t MockTradingAdapter::recovery_fill_count() const noexcept {
  return storage_ ? storage_->fill_count : 0;
}

std::size_t MockTradingAdapter::recovery_event_count() const noexcept {
  return storage_ ? storage_->event_count : 0;
}

base::Status MockTradingAdapter::ReadRecoveryOrder(
    std::size_t index, model::OrderSnapshot& out,
    model::BrokerOrderId* request_key) const noexcept {
  if (!storage_ || index >= storage_->order_count)
    return {base::ErrorCode::kInvalidArgument, {}};
  const auto& order = storage_->orders[index];
  model::OrderSnapshot snapshot;
  snapshot.client_order_id = order.id;
  snapshot.broker_order_id = order.broker_id;
  snapshot.instrument_id = order.request.instrument_id;
  snapshot.side = order.request.side;
  snapshot.limit_price = order.request.limit_price;
  snapshot.original_qty = order.request.quantity;
  snapshot.cumulative_filled_qty = order.cumulative_qty;
  snapshot.state = order.state;
  snapshot.leaves_qty = model::IsTerminalOrderState(order.state)
                            ? 0
                            : order.request.quantity - order.cumulative_qty;
  snapshot.cancel_pending = order.cancel_id.value != 0;
  snapshot.cancel_request_id = order.cancel_id;
  snapshot.report_sequence = order.report_sequence;
  // Report history remains in its original monotonic domain until callbacks
  // resume; it is evidence, not a deadline to rebase.
  for (std::size_t event_index = storage_->event_count; event_index != 0;
       --event_index) {
    const auto& event = storage_->events[event_index - 1];
    if (event.client_order_id == order.id) {
      snapshot.receive_time_ns = event.receive_time_ns;
      break;
    }
  }
  out = snapshot;
  if (request_key) *request_key = order.request_key;
  return {};
}

base::Status MockTradingAdapter::ReadRecoveryFill(
    std::size_t index, model::Fill& out) const noexcept {
  if (!storage_ || index >= storage_->fill_count)
    return {base::ErrorCode::kInvalidArgument, {}};
  out = storage_->events[storage_->fill_events[index]].fill;
  return {};
}

base::Status MockTradingAdapter::ReadRecoveryEvent(
    std::size_t index, TradingEvent& out) const noexcept {
  if (!storage_ || index >= storage_->event_count)
    return {base::ErrorCode::kInvalidArgument, {}};
  out = storage_->events[index];
  return {};
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
  observer_ = nullptr;
  observer_context_ = nullptr;
  return {};
}

}  // namespace yquant::strategyd
