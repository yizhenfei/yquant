#include "strategyd/remote_trading.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <thread>
#include <utility>

#include "yquant/trading/trading_limits.h"

namespace yquant::strategyd {
namespace {
using base::ErrorCode;
using model::OrderState;
using model::OrderUpdateKind;
constexpr std::uint64_t kHeartbeatIntervalNs = 250000000;
// Idle polls yield the current batch while keeping the low-latency event loop
// polling. A future deadline also satisfies the runner's empty-read contract.
constexpr std::uint64_t kPollRetryNs = 1;
constexpr std::size_t kExecutionLookupCapacity = 2 * trading::kMaxTradingFills;

std::uint64_t SaturatingAdd(std::uint64_t value,
                            std::uint64_t amount) noexcept {
  return value > UINT64_MAX - amount ? UINT64_MAX : value + amount;
}

bool ValidId(model::InstrumentId id) noexcept {
  const auto venue = id.value >> 32;
  return venue >= 1 && venue <= 3 && (id.value & UINT32_MAX) <= 999999;
}

bool SameAccount(const trading::AccountView& lhs,
                 const trading::AccountView& rhs) noexcept {
  if (lhs.revision != rhs.revision || lhs.cash != rhs.cash ||
      lhs.position_count != rhs.position_count)
    return false;
  for (std::size_t index = 0; index < lhs.position_count; ++index) {
    if (lhs.positions[index] != rhs.positions[index]) return false;
  }
  return true;
}

bool SameFill(const model::Fill& lhs, const model::Fill& rhs) noexcept {
  return lhs.client_order_id == rhs.client_order_id &&
         lhs.broker_order_id == rhs.broker_order_id &&
         lhs.execution_id == rhs.execution_id &&
         lhs.instrument_id == rhs.instrument_id && lhs.side == rhs.side &&
         lhs.price == rhs.price && lhs.quantity == rhs.quantity &&
         lhs.event_time_ns == rhs.event_time_ns &&
         lhs.has_event_time == rhs.has_event_time;
}

std::size_t ExecutionHash(model::ClientOrderId order,
                          const model::ExecutionId& execution) noexcept {
  std::uint64_t hash =
      (14695981039346656037ULL ^ order.value) * 1099511628211ULL;
  for (const unsigned char byte : execution.view())
    hash = (hash ^ byte) * 1099511628211ULL;
  return hash & (kExecutionLookupCapacity - 1);
}
}  // namespace

struct RemoteTrading::Storage {
  struct OrderRecord {
    model::OrderSnapshot snapshot;
    model::OrderUpdate last_wire_update;
    model::Fill last_wire_fill;
    bool last_notify = false;
    bool last_has_fill = false;
    bool attempted = false;
    bool cancel_attempted = false;
  };
  struct Request {
    std::size_t index = 0;
    bool cancel = false;
    model::CancelRequestId cancel_id;
  };
  std::array<OrderRecord, trading::kMaxTradingOrders> orders = {};
  std::array<Request, trading::kMaxTradingRequests> requests = {};
  std::array<std::size_t, trading::kMaxLocalOrderUpdates> local_updates = {};
  std::array<model::Fill, trading::kMaxTradingFills> fills = {};
  std::array<std::uint16_t, kExecutionLookupCapacity> fill_lookup = {};
  trading::AccountView account;
  trading::RemoteTradingConfig config;
  std::size_t order_count = 0;
  std::size_t request_head = 0;
  std::size_t request_count = 0;
  std::size_t local_head = 0;
  std::size_t local_count = 0;
  std::size_t fill_count = 0;
  std::uint64_t cancel_id = 0;
  std::uint64_t generation = 0;
  std::uint64_t last_received_ns = 0;
  std::uint64_t next_heartbeat_ns = 0;
  std::uint64_t next_poll_ns = 0;
  bool stop_sent = false;
  bool drained = false;
  bool failed = false;
};

RemoteTrading::RemoteTrading(const base::MonotonicClock& clock,
                             const md::InstrumentScope& instruments) noexcept
    : clock_(clock), instruments_(instruments) {}
RemoteTrading::~RemoteTrading() noexcept { Reset(); }

ErrorCode RemoteTrading::Fail(ErrorCode code, const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return code;
}

base::Status RemoteTrading::FailSession(base::Status status,
                                        const char* detail) noexcept {
  if (storage_ != nullptr) storage_->failed = true;
  (void)Fail(status.code, detail);
  return status;
}

base::Status RemoteTrading::Init(
    const trading::TradingConfig& config,
    const md::MarketDataConfig& dictionary) noexcept {
  if (storage_ != nullptr || phase_ != strategy::CallbackPhase::kNone)
    return {
        Fail(ErrorCode::kInvalidState, "remote trading already initialized"),
        {}};
  base::ConfigError error;
  auto status = trading::ValidateTradingConfig(config, error);
  if (!status.ok() || config.adapter != trading::TradingAdapterKind::kTcp ||
      dictionary.instrument_count == 0 ||
      dictionary.instrument_count > trading::kMaxInstruments)
    return {Fail(ErrorCode::kInvalidArgument,
                 "invalid remote endpoint or instrument dictionary"),
            {}};
  storage_.reset(new (std::nothrow) Storage);
  if (storage_ == nullptr)
    return {Fail(ErrorCode::kResourceExhausted,
                 "cannot allocate fixed remote trading storage"),
            {}};
  storage_->config = config.remote;
  status = connection_.Connect(config.remote.endpoint,
                               config.remote.connect_timeout_ns);
  if (!status.ok()) {
    (void)Fail(status.code, connection_.error_detail());
    connection_.Close();
    storage_.reset();
    return status;
  }
  trading::Message hello;
  hello.type = trading::MessageType::kHello;
  hello.client_id = config.remote.client_id;
  std::memcpy(hello.token, config.remote.token, sizeof(hello.token));
  hello.instrument_count = dictionary.instrument_count;
  for (std::size_t index = 0; index < hello.instrument_count; ++index) {
    const auto& definition = dictionary.instruments[index].instrument;
    hello.instruments[index] = {definition.id, definition.price_decimals};
  }
  status = connection_.Queue(hello);
  if (!status.ok()) {
    (void)Fail(status.code, connection_.error_detail());
    connection_.Close();
    storage_.reset();
    return status;
  }
  const auto start = std::chrono::steady_clock::now();
  trading::Message welcome;
  for (;;) {
    const auto received = connection_.Poll(welcome);
    if (received.state == trading::ReceiveState::kMessage) break;
    if (received.state != trading::ReceiveState::kIdle) {
      status = received.status.ok() ? base::Status{ErrorCode::kIoError, {}}
                                    : received.status;
      (void)Fail(status.code, "remote trading handshake failed");
      connection_.Close();
      storage_.reset();
      return status;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (static_cast<std::uint64_t>(elapsed) >=
        config.remote.connect_timeout_ns) {
      connection_.Close();
      storage_.reset();
      return {Fail(ErrorCode::kIoError, "remote trading welcome timed out"),
              {}};
    }
    std::this_thread::yield();
  }
  bool valid = welcome.type == trading::MessageType::kWelcome &&
               welcome.generation != 0 &&
               welcome.instrument_count == hello.instrument_count;
  for (std::size_t index = 0; valid && index < hello.instrument_count;
       ++index) {
    std::size_t matches = 0;
    for (std::size_t remote = 0; remote < welcome.instrument_count; ++remote) {
      if (welcome.instruments[remote].id == hello.instruments[index].id &&
          welcome.instruments[remote].price_decimals ==
              hello.instruments[index].price_decimals)
        ++matches;
    }
    valid = matches == 1;
  }
  if (!valid) {
    connection_.Close();
    storage_.reset();
    return {Fail(ErrorCode::kCorrupt,
                 "remote welcome rejected or instrument dictionary mismatch"),
            {}};
  }
  const auto account = ValidateAccount(welcome.account, true);
  if (account != ErrorCode::kOk) {
    connection_.Close();
    storage_.reset();
    return {account, {}};
  }
  auto& s = *storage_;
  s.account = welcome.account;
  s.generation = welcome.generation;
  s.last_received_ns = clock_.Now();
  s.next_poll_ns = s.last_received_ns;
  s.next_heartbeat_ns = SaturatingAdd(s.last_received_ns, kHeartbeatIntervalNs);
  error_detail_[0] = '\0';
  return {};
}

void RemoteTrading::Reset() noexcept {
  connection_.Close();
  storage_.reset();
  running_ = false;
  stopping_ = false;
  phase_ = strategy::CallbackPhase::kNone;
  error_detail_[0] = '\0';
}

void RemoteTrading::BeginStopping() noexcept {
  running_ = false;
  stopping_ = true;
}

bool RemoteTrading::accepting_orders() const noexcept {
  const bool business = phase_ == strategy::CallbackPhase::kTick ||
                        phase_ == strategy::CallbackPhase::kOrderUpdate ||
                        phase_ == strategy::CallbackPhase::kFill;
  return storage_ != nullptr && !storage_->failed && running_ && !stopping_ &&
         business;
}

ErrorCode RemoteTrading::SubmitOrder(const model::OrderRequest& request,
                                     model::ClientOrderId& out) noexcept {
  if (!accepting_orders())
    return Fail(ErrorCode::kInvalidState,
                "orders require a running business callback");
  model::InstrumentDefinition definition;
  if (!instruments_.TryGetInstrument(request.instrument_id, definition) ||
      (request.side != model::OrderSide::kBuy &&
       request.side != model::OrderSide::kSell) ||
      request.limit_price <= 0 || request.quantity <= 0 ||
      definition.price_decimals > 6)
    return Fail(ErrorCode::kInvalidArgument, "invalid limit-order request");
  constexpr std::array<std::int64_t, 7> kPriceScales = {
      1000000, 100000, 10000, 1000, 100, 10, 1};
  const auto scale = kPriceScales[definition.price_decimals];
  if (request.limit_price > INT64_MAX / request.quantity ||
      request.limit_price * request.quantity > INT64_MAX / scale)
    return Fail(ErrorCode::kInvalidArgument,
                "limit-order cash amount overflows");
  auto& s = *storage_;
  if (s.order_count == s.orders.size() || s.request_count == s.requests.size())
    return Fail(ErrorCode::kResourceExhausted,
                "order or local request capacity exhausted");
  const auto index = s.order_count;
  auto& order = s.orders[index].snapshot;
  order.client_order_id = {index + 1};
  order.instrument_id = request.instrument_id;
  order.side = request.side;
  order.limit_price = request.limit_price;
  order.original_qty = request.quantity;
  order.leaves_qty = request.quantity;
  s.requests[(s.request_head + s.request_count) % s.requests.size()] = {
      index, false, {}};
  ++s.request_count;
  ++s.order_count;
  out = order.client_order_id;
  return ErrorCode::kOk;
}

ErrorCode RemoteTrading::CancelOrder(model::ClientOrderId id) noexcept {
  if (!accepting_orders())
    return Fail(ErrorCode::kInvalidState,
                "cancellation requires a running business callback");
  auto& s = *storage_;
  if (id.value == 0 || id.value > s.order_count)
    return Fail(ErrorCode::kInvalidArgument, "unknown client order ID");
  auto& record = s.orders[id.value - 1];
  if (record.snapshot.state != OrderState::kWorking ||
      record.snapshot.cancel_pending)
    return Fail(ErrorCode::kInvalidState, "order is not cancellable");
  if (s.request_count == s.requests.size() || s.cancel_id == UINT64_MAX)
    return Fail(ErrorCode::kResourceExhausted,
                "cancel request capacity or ID exhausted");
  const model::CancelRequestId cancel_id{++s.cancel_id};
  s.requests[(s.request_head + s.request_count) % s.requests.size()] = {
      static_cast<std::size_t>(id.value - 1), true, cancel_id};
  ++s.request_count;
  record.snapshot.cancel_pending = true;
  record.snapshot.cancel_request_id = cancel_id;
  record.cancel_attempted = false;
  return ErrorCode::kOk;
}

bool RemoteTrading::TryGetOrder(model::ClientOrderId id,
                                model::OrderSnapshot& out) const noexcept {
  if (storage_ == nullptr || id.value == 0 || id.value > storage_->order_count)
    return false;
  out = storage_->orders[id.value - 1].snapshot;
  return true;
}

bool RemoteTrading::TryGetCash(model::CashSnapshot& out) const noexcept {
  if (storage_ == nullptr) return false;
  out = storage_->account.cash;
  return true;
}

bool RemoteTrading::TryGetPosition(
    model::InstrumentId id, model::PositionSnapshot& out) const noexcept {
  if (storage_ == nullptr || !instruments_.HasInstrument(id)) return false;
  for (std::size_t index = 0; index < storage_->account.position_count;
       ++index) {
    if (storage_->account.positions[index].instrument_id == id) {
      out = storage_->account.positions[index];
      return true;
    }
  }
  out = {id, 0, 0, 0};
  return true;
}

base::Status RemoteTrading::ProcessRequests(trading::TradingAdapter&,
                                            std::uint64_t now,
                                            std::size_t limit) noexcept {
  if (phase_ != strategy::CallbackPhase::kNone)
    return {Fail(ErrorCode::kInvalidState, "cannot send inside a callback"),
            {}};
  if (storage_ == nullptr) return {};
  auto& s = *storage_;
  if (s.failed)
    return {Fail(ErrorCode::kInvalidState, "remote trading session has failed"),
            {}};
  if (now < s.last_received_ns ||
      now - s.last_received_ns >= s.config.heartbeat_timeout_ns)
    return FailSession({ErrorCode::kIoError, {}},
                       "remote trading heartbeat timed out");
  auto status = connection_.Service();
  if (!status.ok()) return FailSession(status, connection_.error_detail());
  for (std::size_t count = 0; count < limit && s.request_count != 0; ++count) {
    const auto request = s.requests[s.request_head];
    auto& record = s.orders[request.index];
    if (stopping_ && !request.cancel && !record.attempted) {
      if (s.local_count == s.local_updates.size()) break;
      s.local_updates[(s.local_head + s.local_count) % s.local_updates.size()] =
          request.index;
      ++s.local_count;
    } else {
      if (!connection_.can_queue()) break;
      trading::Message message;
      message.generation = s.generation;
      message.order_id = record.snapshot.client_order_id;
      if (request.cancel) {
        message.type = trading::MessageType::kCancelOrder;
        message.cancel_id = request.cancel_id;
        message.broker_order_id = record.snapshot.broker_order_id;
      } else {
        message.type = trading::MessageType::kNewOrder;
        const auto& order = record.snapshot;
        message.request = {order.instrument_id, order.side, order.limit_price,
                           order.original_qty};
      }
      status = connection_.Queue(message);
      if (!status.ok()) return FailSession(status, connection_.error_detail());
      // Queued transport commands are already uncertain on disconnect and
      // cannot be converted to a local cancellation or resent.
      if (request.cancel)
        record.cancel_attempted = true;
      else
        record.attempted = true;
    }
    s.request_head = (s.request_head + 1) % s.requests.size();
    --s.request_count;
  }
  if (stopping_ && s.request_count == 0 && !s.stop_sent &&
      connection_.can_queue()) {
    trading::Message stop;
    stop.type = trading::MessageType::kStop;
    stop.generation = s.generation;
    status = connection_.Queue(stop);
    if (!status.ok()) return FailSession(status, connection_.error_detail());
    s.stop_sent = true;
  }
  if (!s.drained && now >= s.next_heartbeat_ns && connection_.can_queue()) {
    trading::Message heartbeat;
    heartbeat.generation = s.generation;
    status = connection_.Queue(heartbeat);
    if (!status.ok()) return FailSession(status, connection_.error_detail());
    s.next_heartbeat_ns = SaturatingAdd(now, kHeartbeatIntervalNs);
  }
  status = connection_.Service();
  if (!status.ok()) return FailSession(status, connection_.error_detail());
  return {};
}

base::Status RemoteTrading::ScheduleStopping(std::uint64_t,
                                             std::size_t) noexcept {
  // The gateway owns automatic cancellation and reports completion via Drained.
  return {};
}

std::optional<std::uint64_t> RemoteTrading::next_deadline() const noexcept {
  if (storage_ == nullptr) return {};
  const auto& s = *storage_;
  if (s.request_count != 0 || s.local_count != 0 || (stopping_ && !s.stop_sent))
    return 0;
  return s.next_poll_ns;
}

bool RemoteTrading::has_local_updates() const noexcept {
  return storage_ != nullptr && storage_->local_count != 0;
}

bool RemoteTrading::is_drained() const noexcept {
  if (storage_ == nullptr) return true;
  const auto& s = *storage_;
  if (!s.drained || s.request_count != 0 || s.local_count != 0 ||
      !connection_.empty())
    return false;
  for (std::size_t index = 0; index < s.order_count; ++index) {
    if (!model::IsTerminalOrderState(s.orders[index].snapshot.state) ||
        s.orders[index].snapshot.cancel_pending)
      return false;
  }
  return true;
}

std::size_t RemoteTrading::order_count() const noexcept {
  return storage_ == nullptr ? 0 : storage_->order_count;
}

ErrorCode RemoteTrading::ApplyEvent(const trading::TradingEvent&,
                                    trading::TradingDispatch&) noexcept {
  return Fail(ErrorCode::kInvalidState,
              "remote runtime does not accept local adapter events");
}

ErrorCode RemoteTrading::ApplyLocalUpdate(
    trading::TradingDispatch& out) noexcept {
  if (storage_ == nullptr || storage_->local_count == 0)
    return Fail(ErrorCode::kInvalidState, "no local cancellation available");
  auto& s = *storage_;
  auto& record = s.orders[s.local_updates[s.local_head]];
  if (record.attempted || record.snapshot.state != OrderState::kPendingNew)
    return Fail(ErrorCode::kCorrupt, "invalid pending local cancellation");
  trading::TradingDispatch dispatch;
  dispatch.notify_order = true;
  dispatch.update.kind = OrderUpdateKind::kCanceledBeforeSend;
  dispatch.update.order = record.snapshot;
  dispatch.update.order.state = OrderState::kCanceled;
  dispatch.update.order.leaves_qty = 0;
  dispatch.update.reason.Set(0, "canceled before sending during shutdown");
  record.snapshot = dispatch.update.order;
  s.local_head = (s.local_head + 1) % s.local_updates.size();
  --s.local_count;
  out = dispatch;
  return ErrorCode::kOk;
}

ErrorCode RemoteTrading::ValidateAccount(const trading::AccountView& account,
                                         bool initial) noexcept {
  const auto& cash = account.cash;
  if (account.revision == 0 || cash.balance < 0 || cash.reserved < 0 ||
      cash.reserved > cash.balance ||
      cash.available != cash.balance - cash.reserved ||
      account.position_count > account.positions.size())
    return Fail(ErrorCode::kCorrupt, "invalid authoritative cash or revision");
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    if (!ValidId(position.instrument_id) || position.total_qty < 0 ||
        position.available_qty < 0 || position.reserved_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty > position.total_qty - position.available_qty)
      return Fail(ErrorCode::kCorrupt, "invalid authoritative position");
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id)
        return Fail(ErrorCode::kCorrupt, "duplicate authoritative position");
    }
  }
  if (!initial && (account.revision < storage_->account.revision ||
                   (account.revision == storage_->account.revision &&
                    !SameAccount(account, storage_->account))))
    return Fail(ErrorCode::kCorrupt,
                "authoritative account revision regressed or changed");
  return ErrorCode::kOk;
}

ErrorCode RemoteTrading::ApplyUpdate(const trading::Message& message,
                                     std::uint64_t now,
                                     trading::TradingDispatch& out) noexcept {
  auto& s = *storage_;
  const auto& wire = message.update;
  const auto& next = wire.order;
  const auto id = next.client_order_id;
  if (id.value == 0 || id.value > s.order_count)
    return Fail(ErrorCode::kCorrupt,
                "remote report belongs to an unknown order");
  auto& record = s.orders[id.value - 1];
  const auto& old = record.snapshot;
  if (!record.attempted || next.report_sequence == 0 ||
      next.instrument_id != old.instrument_id || next.side != old.side ||
      next.limit_price != old.limit_price ||
      next.original_qty != old.original_qty ||
      next.cumulative_filled_qty < old.cumulative_filled_qty ||
      next.cumulative_filled_qty > old.original_qty ||
      (!old.broker_order_id.empty() &&
       next.broker_order_id != old.broker_order_id))
    return Fail(ErrorCode::kCorrupt, "remote report changed order identity");
  auto account_status = ValidateAccount(message.account, false);
  if (account_status != ErrorCode::kOk) return account_status;
  if (next.report_sequence == old.report_sequence) {
    auto previous = record.last_wire_update.order;
    auto duplicate = next;
    previous.receive_time_ns = 0;
    duplicate.receive_time_ns = 0;
    if (previous != duplicate || wire.kind != record.last_wire_update.kind ||
        wire.cancel_request_id != record.last_wire_update.cancel_request_id ||
        wire.reason != record.last_wire_update.reason ||
        message.has_fill != record.last_has_fill ||
        message.notify_order != record.last_notify ||
        (message.has_fill && !SameFill(message.fill, record.last_wire_fill)))
      return Fail(ErrorCode::kCorrupt,
                  "remote duplicate report changed content");
    s.account = message.account;
    out = {};
    return ErrorCode::kOk;
  }
  if (next.report_sequence != old.report_sequence + 1)
    return Fail(ErrorCode::kCorrupt, "remote order report sequence gap");
  const bool cancel_result = wire.kind == OrderUpdateKind::kCanceled ||
                             wire.kind == OrderUpdateKind::kCancelRejected;
  if (cancel_result && wire.cancel_request_id.value != 0 &&
      (!old.cancel_pending || !record.cancel_attempted ||
       wire.cancel_request_id != old.cancel_request_id))
    return Fail(ErrorCode::kCorrupt, "remote cancellation identity mismatch");
  if (cancel_result && wire.cancel_request_id.value == 0 &&
      old.cancel_pending && old.cancel_request_id.value != 0)
    return Fail(ErrorCode::kCorrupt, "unexpected gateway cancellation");
  if (!cancel_result && wire.cancel_request_id.value != 0)
    return Fail(ErrorCode::kCorrupt,
                "non-cancel report carries cancellation ID");
  if (message.has_fill != (wire.kind == OrderUpdateKind::kFill))
    return Fail(ErrorCode::kCorrupt, "remote fill notification mismatch");
  if (wire.kind != OrderUpdateKind::kFill &&
      next.cumulative_filled_qty != old.cumulative_filled_qty)
    return Fail(ErrorCode::kCorrupt, "non-fill changed cumulative quantity");
  bool valid = false;
  switch (wire.kind) {
    case OrderUpdateKind::kAccepted:
      valid = !next.broker_order_id.empty() &&
              (old.state == OrderState::kPendingNew ||
               old.state == OrderState::kWorking ||
               model::IsTerminalOrderState(old.state)) &&
              old.state != OrderState::kRejected &&
              next.state == (old.state == OrderState::kPendingNew
                                 ? OrderState::kWorking
                                 : old.state);
      break;
    case OrderUpdateKind::kRejected:
      valid = old.state == OrderState::kPendingNew &&
              old.cumulative_filled_qty == 0 &&
              next.state == OrderState::kRejected;
      break;
    case OrderUpdateKind::kFill:
      valid = !model::IsTerminalOrderState(old.state) &&
              !next.broker_order_id.empty() &&
              next.state == (next.cumulative_filled_qty == old.original_qty
                                 ? OrderState::kFilled
                                 : OrderState::kWorking);
      break;
    case OrderUpdateKind::kCanceled:
      valid = old.state == OrderState::kWorking &&
              next.state == OrderState::kCanceled;
      break;
    case OrderUpdateKind::kCancelRejected:
      valid = (old.state == OrderState::kWorking ||
               old.state == OrderState::kFilled) &&
              next.state == old.state;
      break;
    case OrderUpdateKind::kCanceledBeforeSend:
      break;
  }
  const auto expected_leaves =
      model::IsTerminalOrderState(next.state)
          ? 0
          : old.original_qty - next.cumulative_filled_qty;
  if (!valid || next.leaves_qty != expected_leaves ||
      (cancel_result && next.cancel_pending))
    return Fail(ErrorCode::kCorrupt, "remote order transition is inconsistent");
  const bool expected_notify = wire.kind != OrderUpdateKind::kAccepted ||
                               old.state == OrderState::kPendingNew;
  if (message.notify_order != expected_notify)
    return Fail(ErrorCode::kCorrupt,
                "remote order callback flag is inconsistent");
  std::size_t fill_slot = 0;
  if (message.has_fill) {
    const auto& fill = message.fill;
    if (fill.client_order_id != id || fill.instrument_id != old.instrument_id ||
        fill.side != old.side || fill.broker_order_id != next.broker_order_id ||
        fill.execution_id.empty() || fill.price <= 0 || fill.quantity <= 0 ||
        fill.quantity > old.original_qty - old.cumulative_filled_qty ||
        next.cumulative_filled_qty !=
            old.cumulative_filled_qty + fill.quantity ||
        (!fill.has_event_time && fill.event_time_ns != 0) ||
        (old.side == model::OrderSide::kBuy && fill.price > old.limit_price) ||
        (old.side == model::OrderSide::kSell && fill.price < old.limit_price))
      return Fail(ErrorCode::kCorrupt, "remote execution is inconsistent");
    if (s.fill_count == s.fills.size())
      return Fail(ErrorCode::kResourceExhausted,
                  "execution capacity exhausted");
    fill_slot = ExecutionHash(id, fill.execution_id);
    while (s.fill_lookup[fill_slot] != 0) {
      const auto& previous = s.fills[s.fill_lookup[fill_slot] - 1];
      if (previous.client_order_id == id &&
          previous.execution_id == fill.execution_id)
        return Fail(ErrorCode::kCorrupt, "remote execution ID repeated");
      fill_slot = (fill_slot + 1) & (kExecutionLookupCapacity - 1);
    }
  }
  if (old.broker_order_id.empty() && !next.broker_order_id.empty()) {
    for (std::size_t index = 0; index < s.order_count; ++index) {
      if (index != id.value - 1 &&
          s.orders[index].snapshot.broker_order_id == next.broker_order_id)
        return Fail(ErrorCode::kCorrupt, "broker ID belongs to another order");
    }
  }
  trading::TradingDispatch dispatch;
  dispatch.notify_order = message.notify_order;
  dispatch.has_fill = message.has_fill;
  dispatch.update = wire;
  dispatch.update.order.receive_time_ns = now;
  if (!cancel_result && old.cancel_pending) {
    dispatch.update.order.cancel_pending = true;
    dispatch.update.order.cancel_request_id = old.cancel_request_id;
  }
  if (message.has_fill) {
    dispatch.fill = message.fill;
    dispatch.fill.receive_time_ns = now;
    s.fills[s.fill_count] = dispatch.fill;
    s.fill_lookup[fill_slot] = static_cast<std::uint16_t>(s.fill_count + 1);
    ++s.fill_count;
  }
  record.snapshot = dispatch.update.order;
  record.last_wire_update = wire;
  record.last_wire_fill = message.fill;
  record.last_has_fill = message.has_fill;
  record.last_notify = message.notify_order;
  if (cancel_result) record.cancel_attempted = false;
  s.account = message.account;
  out = dispatch;
  return ErrorCode::kOk;
}

base::Status RemoteTrading::Poll(std::uint64_t now,
                                 trading::TradingDispatch& out,
                                 bool& event) noexcept {
  if (storage_ == nullptr)
    return {Fail(ErrorCode::kInvalidState, "remote trading is disabled"), {}};
  auto& s = *storage_;
  if (s.failed)
    return {Fail(ErrorCode::kInvalidState, "remote trading session has failed"),
            {}};
  if (now < s.last_received_ns)
    return FailSession({ErrorCode::kCorrupt, {}},
                       "remote polling clock regressed");
  trading::Message message;
  const auto received = connection_.Poll(message);
  if (received.state == trading::ReceiveState::kIdle) {
    s.next_poll_ns = SaturatingAdd(now, kPollRetryNs);
    event = false;
    return {};
  }
  if (received.state != trading::ReceiveState::kMessage) {
    const auto code =
        received.status.ok() ? ErrorCode::kIoError : received.status.code;
    return FailSession({code, received.status.system_error},
                       "remote trading stream closed or failed");
  }
  if (message.generation != s.generation)
    return FailSession({ErrorCode::kCorrupt, {}},
                       "remote trading generation changed");
  if (s.drained && message.type != trading::MessageType::kHeartbeat)
    return FailSession({ErrorCode::kCorrupt, {}},
                       "remote update followed drain acknowledgement");
  trading::TradingDispatch dispatch;
  auto code = ErrorCode::kOk;
  bool notification = false;
  switch (message.type) {
    case trading::MessageType::kOrderUpdate:
      code = ApplyUpdate(message, now, dispatch);
      notification = dispatch.notify_order || dispatch.has_fill;
      break;
    case trading::MessageType::kAccount:
      code = ValidateAccount(message.account, false);
      if (code == ErrorCode::kOk) s.account = message.account;
      break;
    case trading::MessageType::kHeartbeat:
      break;
    case trading::MessageType::kDrained:
      if (!stopping_ || !s.stop_sent || s.drained)
        code = Fail(ErrorCode::kCorrupt,
                    "unexpected remote drain acknowledgement");
      else {
        for (std::size_t index = 0; index < s.order_count; ++index) {
          const auto& record = s.orders[index];
          if (record.attempted &&
              (!model::IsTerminalOrderState(record.snapshot.state) ||
               record.snapshot.cancel_pending)) {
            code = Fail(ErrorCode::kCorrupt,
                        "gateway drained with unconfirmed orders");
            break;
          }
        }
        if (code == ErrorCode::kOk) s.drained = true;
      }
      break;
    case trading::MessageType::kError:
      code = Fail(message.error_code == ErrorCode::kOk ? ErrorCode::kCorrupt
                                                       : message.error_code,
                  message.reason.text);
      break;
    default:
      code = Fail(ErrorCode::kCorrupt, "unexpected remote trading message");
      break;
  }
  if (code != ErrorCode::kOk) {
    s.failed = true;
    return {code, {}};
  }
  s.last_received_ns = now;
  s.next_poll_ns = now;
  out = dispatch;
  event = notification;
  return {};
}

}  // namespace yquant::strategyd
