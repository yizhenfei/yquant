#include "yquant/trading/ctp_trading_adapter.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/trading/connection.h"

namespace yquant::trading {
namespace {

// Poll the local bridge without making an idle adapter retain a due deadline.
constexpr uint64_t kPollIntervalNs = 1000000;
// Runtime heartbeats continue while the broker completes account queries.
constexpr uint64_t kHeartbeatIntervalNs = 250000000;
constexpr size_t kReceiveBudget = 16;

uint64_t After(uint64_t now, uint64_t interval) noexcept {
  return interval > UINT64_MAX - now ? UINT64_MAX : now + interval;
}

bool SameAccount(const BrokerAccountSnapshot& a,
                 const BrokerAccountSnapshot& b) noexcept {
  if (a.account != b.account || a.position_count != b.position_count) {
    return false;
  }
  for (size_t i = 0; i < a.position_count; ++i) {
    if (a.positions[i] != b.positions[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

struct CtpTradingAdapter::Storage {
  struct Order {
    model::ClientOrderId id;
    model::OrderRequest request;
    uint64_t sequence = 0;
    bool active = true;
    bool cancel_pending = false;
  };
  CtpTradingConfig config;
  Connection connection;
  BrokerAccountSnapshot account;
  std::array<Order, kMaxTradingOrders> orders{};
  size_t order_count = 0;
  uint64_t generation = 0;
  uint64_t poll_at = 0;
  uint64_t last_receive = 0;
  uint64_t last_send = 0;
  bool started = false;
  bool clock_started = false;
  bool stopping = false;
  bool stop_sent = false;
  bool bridge_drained = false;
  base::Status failure;

  // Client order IDs may be sparse; this bounded cold-sized table owns them
  // until the gateway session ends. No order identity is reused.
  Order* Find(model::ClientOrderId id) noexcept {
    for (size_t i = 0; i < order_count; ++i) {
      if (orders[i].id == id) {
        return &orders[i];
      }
    }
    return nullptr;
  }
  bool LocalDrained() const noexcept {
    for (size_t i = 0; i < order_count; ++i) {
      if (orders[i].active || orders[i].cancel_pending) {
        return false;
      }
    }
    return connection.empty();
  }
};

CtpTradingAdapter::CtpTradingAdapter(
    const market::InstrumentScope* instruments) noexcept
    : instruments_(*instruments) {}
CtpTradingAdapter::~CtpTradingAdapter() noexcept { (void)Stop(); }

base::Status CtpTradingAdapter::Fail(base::ErrorCode code,
                                     const char* detail) noexcept {
  if (storage_ && !storage_->failure.ok()) {
    return storage_->failure;
  }
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  const base::Status status{code, {}};
  if (storage_) {
    storage_->failure = status;
  }
  return status;
}

base::Status CtpTradingAdapter::Prepare(const TradingConfig& config) noexcept {
  if (storage_) {
    return Fail(base::ErrorCode::kInvalidState,
                "CTP bridge adapter is already prepared");
  }
  base::ConfigError error;
  const auto valid = ValidateTradingConfig(config, error);
  if (!valid.ok() || config.adapter != TradingAdapterKind::kCtp) {
    return Fail(base::ErrorCode::kInvalidArgument,
                "CTP bridge requires a valid ctp broker configuration");
  }
  if (instruments_.size() == 0 || instruments_.size() > kMaxInstruments) {
    return Fail(base::ErrorCode::kInvalidArgument,
                "CTP bridge requires a configured futures instrument scope");
  }
  for (size_t i = 0; i < instruments_.size(); ++i) {
    if (instruments_[i].type != model::InstrumentType::kFutures) {
      return Fail(base::ErrorCode::kInvalidArgument,
                  "CTP bridge only supports futures instruments");
    }
  }
  auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!storage) {
    return Fail(base::ErrorCode::kResourceExhausted,
                "cannot allocate CTP bridge storage");
  }
  storage->config = config.ctp;
  storage_ = std::move(storage);
  error_detail_[0] = '\0';
  return {};
}

base::Status CtpTradingAdapter::Start(uint64_t) noexcept {
  if (!storage_ || storage_->started) {
    return Fail(base::ErrorCode::kInvalidState,
                "CTP bridge must be prepared and started only once");
  }
  auto& s = *storage_;
  if (!s.failure.ok()) {
    return s.failure;
  }
  Endpoint endpoint;
  std::memcpy(endpoint.address, s.config.address, sizeof(endpoint.address));
  endpoint.port = s.config.port;
  auto status = s.connection.Connect(endpoint, s.config.connect_timeout_ns);
  if (!status.ok()) {
    return Fail(status.code, s.connection.error_detail());
  }
  Message hello;
  hello.type = MessageType::kHello;
  hello.client_id = 1;
  std::memcpy(hello.token, s.config.token, sizeof(hello.token));
  hello.instrument_count = instruments_.size();
  for (size_t i = 0; i < instruments_.size(); ++i) {
    hello.instruments[i] = {instruments_[i].id, instruments_[i].price_decimals};
  }
  status = s.connection.Queue(hello);
  if (!status.ok()) {
    return Fail(status.code, s.connection.error_detail());
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(s.config.connect_timeout_ns);
  while (std::chrono::steady_clock::now() < deadline) {
    Message incoming;
    const auto received = s.connection.Poll(incoming);
    if (received.state == ReceiveState::kError ||
        received.state == ReceiveState::kClosed) {
      return Fail(base::ErrorCode::kIoError,
                  "CTP bridge closed before completing broker initialization");
    }
    if (received.state == ReceiveState::kIdle) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (incoming.type == MessageType::kHeartbeat) {
      continue;
    }
    if (incoming.type == MessageType::kError) {
      return Fail(incoming.error_code, incoming.reason.text);
    }
    if (incoming.type != MessageType::kWelcome || incoming.generation == 0 ||
        !incoming.account.broker_managed ||
        incoming.account.type != model::AccountType::kFutures ||
        incoming.account.revision == 0 ||
        incoming.instrument_count != instruments_.size()) {
      return Fail(base::ErrorCode::kCorrupt,
                  "CTP bridge welcome has an invalid account or scope");
    }
    for (size_t i = 0; i < instruments_.size(); ++i) {
      size_t matches = 0;
      for (size_t j = 0; j < incoming.instrument_count; ++j) {
        if (incoming.instruments[j].id == instruments_[i].id &&
            incoming.instruments[j].price_decimals ==
                instruments_[i].price_decimals) {
          ++matches;
        }
      }
      if (matches != 1) {
        return Fail(base::ErrorCode::kCorrupt,
                    "CTP bridge instrument dictionary differs from gateway");
      }
    }
    s.account.revision = incoming.account.revision;
    s.account.account = incoming.account.futures;
    s.account.positions = incoming.account.futures_positions;
    s.account.position_count = incoming.account.futures_position_count;
    s.generation = incoming.generation;
    s.started = true;
    return {};
  }
  return Fail(base::ErrorCode::kIoError,
              "CTP bridge login, settlement or account query timed out");
}

base::Status CtpTradingAdapter::ReadAccountSnapshot(
    TradingAccountSnapshot&) noexcept {
  return {base::ErrorCode::kUnsupported, {}};
}

base::Status CtpTradingAdapter::ReadBrokerAccountSnapshot(
    BrokerAccountSnapshot& out) noexcept {
  if (!storage_ || !storage_->started || !storage_->failure.ok()) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  out = storage_->account;
  return {};
}

base::Status CtpTradingAdapter::SendOrder(model::ClientOrderId id,
                                          const model::OrderRequest& request,
                                          uint64_t now) noexcept {
  if (!storage_ || !storage_->started || !storage_->failure.ok()) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& s = *storage_;
  if (s.stopping) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (s.Find(id) != nullptr || s.order_count == s.orders.size()) {
    return Fail(base::ErrorCode::kResourceExhausted,
                "CTP bridge order identity is duplicate or capacity exhausted");
  }
  Message message;
  message.type = MessageType::kNewOrder;
  message.generation = s.generation;
  message.client_order_id = id;
  message.request = request;
  const auto status = s.connection.Queue(message);
  if (!status.ok()) {
    return Fail(status.code, s.connection.error_detail());
  }
  auto& order = s.orders[s.order_count++];
  order.id = id;
  order.request = request;
  s.last_send = now;
  return {};
}

base::Status CtpTradingAdapter::SendCancel(
    model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
    model::CancelRequestId cancel_request_id, uint64_t now) noexcept {
  if (!storage_ || !storage_->started || !storage_->failure.ok()) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& s = *storage_;
  auto* order = s.Find(id);
  if (order == nullptr || order->cancel_pending) {
    return Fail(base::ErrorCode::kInvalidState,
                "CTP cancel has no known order or is already outstanding");
  }
  Message message;
  message.type = MessageType::kCancelOrder;
  message.generation = s.generation;
  message.client_order_id = id;
  message.cancel_id = cancel_request_id;
  message.adapter_order_id = adapter_order_id;
  const auto status = s.connection.Queue(message);
  if (!status.ok()) {
    return Fail(status.code, s.connection.error_detail());
  }
  order->cancel_pending = true;
  s.last_send = now;
  return {};
}

base::Status CtpTradingAdapter::ServiceConnection(uint64_t now) noexcept {
  auto& s = *storage_;
  if (!s.clock_started) {
    s.last_receive = s.last_send = now;
    s.clock_started = true;
  }
  s.poll_at = After(now, kPollIntervalNs);
  if (s.stopping && !s.stop_sent && s.LocalDrained()) {
    Message stop;
    stop.type = MessageType::kStop;
    stop.generation = s.generation;
    const auto status = s.connection.Queue(stop);
    if (!status.ok()) {
      return Fail(status.code, s.connection.error_detail());
    }
    s.stop_sent = true;
    s.last_send = now;
  }
  if (now - s.last_receive >= s.config.heartbeat_timeout_ns) {
    return Fail(base::ErrorCode::kIoError, "CTP bridge heartbeat timed out");
  }
  // Once Stop is sent the bridge may finish and close at any time. Keep
  // reading its final account/Drained frames without racing a new write
  // against that expected close. Stop itself refreshed the peer deadline.
  if (!s.stop_sent && now - s.last_send >= kHeartbeatIntervalNs) {
    Message heartbeat;
    heartbeat.type = MessageType::kHeartbeat;
    heartbeat.generation = s.generation;
    const auto status = s.connection.Queue(heartbeat);
    if (!status.ok()) {
      return Fail(status.code, s.connection.error_detail());
    }
    s.last_send = now;
  }
  return {};
}

TradingPollResult CtpTradingAdapter::ApplyBridgeMessage(
    const Message& message, uint64_t now, model::TradingEvent& out) noexcept {
  auto& s = *storage_;
  if (message.type == MessageType::kDrained) {
    if (!s.stop_sent || !s.LocalDrained()) {
      return {TradingPollState::kError,
              Fail(base::ErrorCode::kCorrupt,
                   "CTP bridge drained before local orders settled")};
    }
    s.bridge_drained = true;
    return {};
  }
  if (message.type == MessageType::kError) {
    return {TradingPollState::kError,
            Fail(message.error_code, message.reason.text)};
  }
  if ((message.type != MessageType::kOrderUpdate &&
       message.type != MessageType::kAccount) ||
      !message.account.broker_managed ||
      message.account.type != model::AccountType::kFutures ||
      message.account.revision == 0) {
    return {TradingPollState::kError,
            Fail(base::ErrorCode::kCorrupt, "unexpected CTP bridge message")};
  }
  BrokerAccountSnapshot next;
  next.revision = message.account.revision;
  next.account = message.account.futures;
  next.positions = message.account.futures_positions;
  next.position_count = message.account.futures_position_count;
  if (next.revision < s.account.revision ||
      (next.revision == s.account.revision && !SameAccount(next, s.account))) {
    return {TradingPollState::kError,
            Fail(base::ErrorCode::kCorrupt,
                 "CTP broker account revision regressed")};
  }
  if (message.type == MessageType::kAccount) {
    s.account = next;
    return {TradingPollState::kAccount, {}};
  }
  return ApplyOrderMessage(message, next, now, out);
}

TradingPollResult CtpTradingAdapter::ApplyOrderMessage(
    const Message& message, const BrokerAccountSnapshot& next, uint64_t now,
    model::TradingEvent& out) noexcept {
  auto& s = *storage_;
  const auto& update = message.update;
  auto* order = s.Find(update.order.client_order_id);
  if (order == nullptr || update.order.strategy_order_id.value != 0 ||
      update.order.instrument_id != order->request.instrument_id ||
      update.order.side != order->request.side ||
      update.order.limit_price != order->request.limit_price ||
      update.order.original_qty != order->request.quantity ||
      update.order.position_effect != order->request.position_effect) {
    return {
        TradingPollState::kError,
        Fail(base::ErrorCode::kCorrupt, "CTP report order identity differs")};
  }
  model::TradingEvent event;
  event.client_order_id = update.order.client_order_id;
  event.adapter_order_id = update.order.adapter_order_id;
  event.report_sequence = update.order.report_sequence;
  event.kind = update.kind;
  event.cumulative_filled_qty = update.order.cumulative_filled_qty;
  event.receive_time_ns = now;
  event.cancel_request_id = update.cancel_request_id;
  event.reason = update.reason;
  if (message.has_fill) {
    event.fill = message.fill;
    event.fill.receive_time_ns = now;
  }
  if (event.report_sequence > order->sequence) {
    order->sequence = event.report_sequence;
    order->active = !model::IsTerminalOrderState(update.order.state);
    order->cancel_pending = update.order.cancel_pending;
  }
  s.account = next;
  out = event;
  return {TradingPollState::kEvent, {}};
}

TradingPollResult CtpTradingAdapter::Poll(uint64_t now,
                                          model::TradingEvent& out) noexcept {
  if (!storage_ || !storage_->started) {
    return {TradingPollState::kError,
            Fail(base::ErrorCode::kInvalidState, "CTP bridge is not started")};
  }
  auto& s = *storage_;
  if (!s.failure.ok()) {
    return {TradingPollState::kError, s.failure};
  }
  if (s.bridge_drained) {
    return {};
  }
  const auto status = ServiceConnection(now);
  if (!status.ok()) {
    return {TradingPollState::kError, status};
  }
  for (size_t i = 0; i < kReceiveBudget; ++i) {
    Message message;
    const auto result = s.connection.Poll(message);
    if (result.state == ReceiveState::kIdle) {
      return {};
    }
    if (result.state == ReceiveState::kError ||
        result.state == ReceiveState::kClosed) {
      return {TradingPollState::kError,
              Fail(base::ErrorCode::kIoError,
                   "CTP bridge disconnected; outstanding requests need "
                   "reconciliation")};
    }
    s.last_receive = now;
    if (message.generation != s.generation) {
      return {TradingPollState::kError,
              Fail(base::ErrorCode::kCorrupt, "CTP bridge generation changed")};
    }
    if (message.type == MessageType::kHeartbeat) {
      continue;
    }
    return ApplyBridgeMessage(message, now, out);
  }
  return {};
}

std::optional<uint64_t> CtpTradingAdapter::next_deadline() const noexcept {
  if (!storage_ || !storage_->started || !storage_->failure.ok() ||
      storage_->bridge_drained) {
    return {};
  }
  return storage_->clock_started ? storage_->poll_at : 0;
}

bool CtpTradingAdapter::is_drained() const noexcept {
  if (!storage_) {
    return true;
  }
  return storage_->LocalDrained() &&
         (!storage_->stopping || storage_->bridge_drained);
}

void CtpTradingAdapter::BeginStopping() noexcept {
  if (storage_) {
    storage_->stopping = true;
  }
}

base::Status CtpTradingAdapter::Stop() noexcept {
  if (!storage_) {
    return {};
  }
  auto& s = *storage_;
  auto status = s.failure;
  if (s.started && status.ok() && !is_drained()) {
    status =
        Fail(base::ErrorCode::kInvalidState,
             "CTP bridge stopped before orders/account drain confirmation");
  }
  s.connection.Close();
  storage_.reset();
  return status;
}

const char* CtpTradingAdapter::error_detail() const noexcept {
  return error_detail_;
}

}  // namespace yquant::trading
