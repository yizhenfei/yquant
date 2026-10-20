#include "tradingd/server.h"

#include <sys/random.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "strategyd/market_data.h"
#include "strategyd/mock_trading_adapter.h"
#include "strategyd/trading.h"
#include "strategyd/trading_adapter.h"
#include "tradingd/connection.h"

namespace yquant::tradingd {
namespace {

inline constexpr std::size_t kRequestsPerClient = 4;
inline constexpr std::size_t kBrokerEventsPerService = 64;
inline constexpr std::size_t kCancelScanPerService = 64;
inline constexpr std::uint64_t kCancelRetryNs = 10000000;

// Client-local identities remain retained even after a disconnected socket.
struct LocalOrder {
  model::ClientOrderId core_id;
  model::OrderSnapshot rejected;
  std::uint64_t report_sequence = 0;
  model::CancelRequestId core_cancel;
  model::CancelRequestId local_cancel;
  std::uint64_t retry_at = 0;
};

struct Client {
  Connection connection;
  std::uint64_t id = 0;
  bool authenticated = false;
  bool stopping = false;
  bool requested_stop = false;
  bool drained_sent = false;
  std::uint64_t last_receive = 0;
  std::uint64_t last_heartbeat = 0;
  std::uint64_t stop_time = 0;
  std::array<InstrumentSpec, kMaxInstruments> instruments = {};
  std::size_t instrument_count = 0;
  std::array<LocalOrder, strategyd::kMaxTradingOrders> orders = {};
  std::size_t order_count = 0;
  std::uint64_t last_cancel_id = 0;
};

struct OrderOwner {
  std::size_t client = 0;
  std::size_t local = 0;
};

// Equality uses all token bytes to avoid a prefix-length timing comparison.
bool SameToken(const char* left, const char* right) noexcept {
  unsigned mismatch = 0;
  for (std::size_t i = 0; i <= kMaxTokenBytes; ++i)
    mismatch |= static_cast<unsigned char>(left[i]) ^
                static_cast<unsigned char>(right[i]);
  return mismatch == 0;
}

bool HasInstrument(const Client& client, model::InstrumentId id) noexcept {
  for (std::size_t i = 0; i < client.instrument_count; ++i)
    if (client.instruments[i].id == id) return true;
  return false;
}

bool ClientDrained(const Client& client,
                   const strategyd::TradingImpl& trading) noexcept {
  for (std::size_t i = 0; i < client.order_count; ++i) {
    if (client.orders[i].core_id.value == 0) continue;
    model::OrderSnapshot snapshot;
    if (!trading.TryGetOrder(client.orders[i].core_id, snapshot) ||
        !model::IsTerminalOrderState(snapshot.state) || snapshot.cancel_pending)
      return false;
  }
  return true;
}

}  // namespace

struct Server::Storage {
  explicit Storage(const strategyd::MonotonicClock& clock) noexcept
      : market_data(clock), trading(clock, market_data) {}
  ServerConfig config;
  Listener listener;
  strategyd::MarketDataImpl market_data;
  strategyd::TradingImpl trading;
  strategyd::MockTradingAdapter adapter;
  std::array<Client, kMaxClients> clients;
  std::array<OrderOwner, strategyd::kMaxTradingOrders> owners = {};
  AccountView account;
  std::uint64_t generation = 0;
  bool stopping = false;
  std::uint64_t stop_time = 0;
  std::size_t cancel_cursor = 0;
  bool account_dirty = false;
};

Server::Server(const strategyd::MonotonicClock& clock) noexcept
    : clock_(clock) {}
Server::~Server() noexcept { Close(); }

base::Status Server::Fail(base::ErrorCode code, const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

base::Status Server::Init(const ServerConfig& config) noexcept {
  Close();
  error_detail_[0] = '\0';
  strategy::ConfigError error;
  auto status = ValidateConfig(config, error);
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return status;
  }
  storage_.reset(new (std::nothrow) Storage(clock_));
  if (!storage_)
    return Fail(base::ErrorCode::kResourceExhausted,
                "server allocation failed");
  auto& s = *storage_;
  s.config = config;
  const auto token_size = std::strlen(s.config.token);
  std::memset(s.config.token + token_size + 1, 0,
              sizeof(s.config.token) - token_size - 1);
  if (getentropy(&s.generation, sizeof(s.generation)) != 0 ||
      s.generation == 0) {
    Close();
    return Fail(base::ErrorCode::kIoError, "cannot create server generation");
  }
  status = s.market_data.Configure(config.market_data);
  if (status.ok()) status = s.adapter.Prepare(config.trading);
  if (status.ok()) status = s.adapter.Start(clock_.Now());
  strategyd::TradingAccountSnapshot initial;
  if (status.ok()) status = s.adapter.ReadAccountSnapshot(initial);
  if (status.ok()) status = s.trading.Configure(initial);
  if (status.ok()) status = s.listener.Open(config.listen);
  if (!status.ok()) {
    Close();
    return Fail(status.code, "cannot initialize trading account or listener");
  }
  s.trading.SetRunning(true);
  s.account.revision = 1;
  RefreshAccount();
  return {};
}

void Server::Disconnect(std::size_t index) noexcept {
  auto& client = storage_->clients[index];
  client.connection.Close();
  if (client.authenticated) {
    client.stopping = true;
  } else {
    client.id = 0;
  }
}

void Server::RefreshAccount() noexcept {
  auto& s = *storage_;
  (void)s.trading.TryGetCash(s.account.cash);
  s.account.position_count = 0;
  for (std::size_t i = 0; i < s.config.market_data.instrument_count; ++i) {
    model::PositionSnapshot position;
    const auto id = s.config.market_data.instruments[i].instrument.id;
    if (s.trading.TryGetPosition(id, position) &&
        (position.total_qty != 0 || position.reserved_qty != 0))
      s.account.positions[s.account.position_count++] = position;
  }
}

void Server::BroadcastAccount() noexcept {
  auto& s = *storage_;
  Message message;
  message.type = MessageType::kAccount;
  message.generation = s.generation;
  message.account = s.account;
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (client.authenticated && !client.drained_sent &&
        client.connection.open() && !client.connection.Queue(message).ok())
      Disconnect(i);
  }
}

base::Status Server::Handle(std::size_t index, const Message& message,
                            std::uint64_t now) noexcept {
  auto& s = *storage_;
  auto& client = s.clients[index];
  if (!client.authenticated) {
    if (message.type != MessageType::kHello || message.generation != 0 ||
        message.client_id == 0 || !SameToken(message.token, s.config.token) ||
        message.instrument_count == 0) {
      Disconnect(index);
      return {};
    }
    for (const auto& other : s.clients) {
      if (other.id == message.client_id) {
        Disconnect(index);
        return {};
      }
    }
    for (std::size_t i = 0; i < message.instrument_count; ++i) {
      model::InstrumentDefinition instrument;
      if (!s.market_data.TryGetInstrument(message.instruments[i].id,
                                          instrument) ||
          instrument.price_decimals != message.instruments[i].price_decimals) {
        Disconnect(index);
        return {};
      }
    }
    client.id = message.client_id;
    client.authenticated = true;
    client.instruments = message.instruments;
    client.instrument_count = message.instrument_count;
    Message welcome;
    welcome.type = MessageType::kWelcome;
    welcome.generation = s.generation;
    welcome.instruments = client.instruments;
    welcome.instrument_count = client.instrument_count;
    welcome.account = s.account;
    if (!client.connection.Queue(welcome).ok()) Disconnect(index);
    return {};
  }
  if (message.generation != s.generation ||
      (message.type != MessageType::kNewOrder &&
       message.type != MessageType::kCancelOrder &&
       message.type != MessageType::kStop &&
       message.type != MessageType::kHeartbeat)) {
    Disconnect(index);
    return {};
  }
  if (message.type == MessageType::kHeartbeat) return {};
  if (message.type == MessageType::kStop) {
    client.requested_stop = true;
    if (!client.stopping) {
      client.stopping = true;
      client.stop_time = now;
    }
    return {};
  }
  if (!client.connection.can_queue()) {
    Disconnect(index);
    return {};
  }
  if (message.type == MessageType::kNewOrder) {
    if (client.stopping || s.stopping ||
        message.order_id.value != client.order_count + 1 ||
        client.order_count == client.orders.size() ||
        !HasInstrument(client, message.request.instrument_id)) {
      Disconnect(index);
      return {};
    }
    const auto local_index = client.order_count++;
    auto& local = client.orders[local_index];
    s.trading.BeginCallback(strategyd::CallbackPhase::kTick);
    const auto code = s.trading.SubmitOrder(message.request, local.core_id);
    s.trading.EndCallback();
    if (code == base::ErrorCode::kOk) {
      s.owners[local.core_id.value - 1] = {index, local_index};
      ++s.account.revision;
      RefreshAccount();
      s.account_dirty = true;
    } else {
      Message reply;
      reply.type = MessageType::kOrderUpdate;
      reply.generation = s.generation;
      reply.account = s.account;
      reply.update.kind = model::OrderUpdateKind::kRejected;
      reply.update.reason.Set(static_cast<std::int32_t>(code),
                              s.trading.error_detail());
      auto& order = reply.update.order;
      order.client_order_id = message.order_id;
      order.instrument_id = message.request.instrument_id;
      order.side = message.request.side;
      order.limit_price = message.request.limit_price;
      order.original_qty = message.request.quantity;
      order.state = model::OrderState::kRejected;
      order.report_sequence = ++local.report_sequence;
      order.receive_time_ns = now;
      local.rejected = order;
      if (!client.connection.Queue(reply).ok()) Disconnect(index);
    }
    return {};
  }
  if (message.order_id.value == 0 ||
      message.order_id.value > client.order_count ||
      message.cancel_id.value == 0 ||
      message.cancel_id.value <= client.last_cancel_id) {
    Disconnect(index);
    return {};
  }
  client.last_cancel_id = message.cancel_id.value;
  auto& local = client.orders[message.order_id.value - 1];
  model::OrderSnapshot snapshot = local.rejected;
  const bool has_core = local.core_id.value != 0;
  if (has_core) (void)s.trading.TryGetOrder(local.core_id, snapshot);
  if (snapshot.cancel_pending) {
    Disconnect(index);
    return {};
  }
  base::ErrorCode code = base::ErrorCode::kInvalidState;
  if (has_core && !client.stopping && !s.stopping &&
      (message.broker_order_id.empty() ||
       message.broker_order_id == snapshot.broker_order_id)) {
    s.trading.BeginCallback(strategyd::CallbackPhase::kTick);
    code = s.trading.CancelOrder(local.core_id);
    s.trading.EndCallback();
  }
  if (code == base::ErrorCode::kOk) {
    (void)s.trading.TryGetOrder(local.core_id, snapshot);
    local.core_cancel = snapshot.cancel_request_id;
    local.local_cancel = message.cancel_id;
  } else {
    // A failed cancel is an explicit result, including after a fill. It never
    // releases reservations or invents a successful broker cancellation.
    Message reply;
    reply.type = MessageType::kOrderUpdate;
    reply.generation = s.generation;
    reply.account = s.account;
    reply.update.order = snapshot;
    reply.update.order.client_order_id = message.order_id;
    reply.update.order.report_sequence = ++local.report_sequence;
    reply.update.order.cancel_pending = false;
    reply.update.order.cancel_request_id = message.cancel_id;
    reply.update.kind = model::OrderUpdateKind::kCancelRejected;
    reply.update.cancel_request_id = message.cancel_id;
    reply.update.reason.Set(static_cast<std::int32_t>(code),
                            "order is not eligible for this cancel request");
    local.core_cancel = snapshot.cancel_request_id;
    local.local_cancel = message.cancel_id;
    if (!has_core) local.rejected = reply.update.order;
    if (!client.connection.Queue(reply).ok()) Disconnect(index);
  }
  return {};
}

base::Status Server::Dispatch(std::uint64_t now) noexcept {
  auto& s = *storage_;
  for (std::size_t count = 0; count < kBrokerEventsPerService; ++count) {
    strategyd::TradingDispatch dispatch;
    base::ErrorCode code;
    if (s.trading.has_local_updates()) {
      code = s.trading.ApplyLocalUpdate(dispatch);
    } else {
      strategyd::TradingEvent event;
      const auto poll = s.adapter.Poll(now, event);
      if (poll.state == strategyd::TradingPollState::kIdle) break;
      if (poll.state == strategyd::TradingPollState::kError)
        return Fail(poll.status.code, s.adapter.error_detail());
      code = s.trading.ApplyEvent(event, dispatch);
    }
    if (code != base::ErrorCode::kOk)
      return Fail(code, s.trading.error_detail());
    if (dispatch.update.order.client_order_id.value == 0) continue;
    const auto core_id = dispatch.update.order.client_order_id.value;
    if (core_id == 0 || core_id > s.trading.order_count())
      return Fail(base::ErrorCode::kCorrupt, "missing order ownership");
    const auto owner = s.owners[core_id - 1];
    auto& client = s.clients[owner.client];
    auto& local = client.orders[owner.local];
    ++s.account.revision;
    RefreshAccount();
    Message message;
    message.type = MessageType::kOrderUpdate;
    message.generation = s.generation;
    message.notify_order = dispatch.notify_order;
    message.has_fill = dispatch.has_fill;
    message.update = dispatch.update;
    message.fill = dispatch.fill;
    message.account = s.account;
    message.update.order.client_order_id.value = owner.local + 1;
    message.update.order.report_sequence = ++local.report_sequence;
    if (dispatch.has_fill) message.fill.client_order_id.value = owner.local + 1;
    const auto wire_cancel =
        message.update.order.cancel_request_id == local.core_cancel
            ? local.local_cancel
            : model::CancelRequestId{};
    message.update.order.cancel_request_id = wire_cancel;
    const bool cancel_report =
        dispatch.update.kind == model::OrderUpdateKind::kCanceled ||
        dispatch.update.kind == model::OrderUpdateKind::kCancelRejected;
    message.update.cancel_request_id =
        cancel_report && dispatch.update.cancel_request_id == local.core_cancel
            ? local.local_cancel
            : model::CancelRequestId{};
    if (client.connection.open() && !client.connection.Queue(message).ok())
      Disconnect(owner.client);
    if (client.connection.open() && !client.connection.Service().ok())
      Disconnect(owner.client);
    s.account_dirty = true;
    if (dispatch.update.kind == model::OrderUpdateKind::kCancelRejected)
      local.retry_at =
          now > UINT64_MAX - kCancelRetryNs ? UINT64_MAX : now + kCancelRetryNs;
  }
  return {};
}

void Server::ScheduleClientCancels(std::uint64_t now) noexcept {
  auto& s = *storage_;
  const auto count = s.trading.order_count();
  for (std::size_t scanned = 0; scanned < kCancelScanPerService && count != 0;
       ++scanned) {
    if (s.cancel_cursor >= count) s.cancel_cursor = 0;
    const auto core_index = s.cancel_cursor++;
    const auto owner = s.owners[core_index];
    auto& client = s.clients[owner.client];
    auto& local = client.orders[owner.local];
    if (!client.stopping || local.retry_at > now) continue;
    model::OrderSnapshot order;
    const model::ClientOrderId id{core_index + 1};
    if (!s.trading.TryGetOrder(id, order) ||
        order.state != model::OrderState::kWorking || order.cancel_pending)
      continue;
    s.trading.BeginCallback(strategyd::CallbackPhase::kTick);
    const auto code = s.trading.CancelOrder(id);
    s.trading.EndCallback();
    if (code == base::ErrorCode::kOk) {
      (void)s.trading.TryGetOrder(id, order);
      local.core_cancel = order.cancel_request_id;
      local.local_cancel = {};
    }
  }
}

void Server::NotifyDrained() noexcept {
  auto& s = *storage_;
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (!client.authenticated || !client.connection.open() ||
        !client.requested_stop || client.drained_sent ||
        !ClientDrained(client, s.trading))
      continue;
    Message message;
    message.type = MessageType::kDrained;
    message.generation = s.generation;
    if (!client.connection.Queue(message).ok())
      Disconnect(i);
    else
      client.drained_sent = true;
  }
}

base::Status Server::Service(std::uint64_t now) noexcept {
  if (!storage_)
    return Fail(base::ErrorCode::kInvalidState, "server is not initialized");
  auto& s = *storage_;
  if (!s.stopping) {
    for (std::size_t i = 0; i < s.config.max_clients; ++i) {
      auto& client = s.clients[i];
      if (client.id != 0 || client.connection.open()) continue;
      bool accepted = false;
      const auto status = s.listener.Accept(client.connection, accepted);
      if (!status.ok()) return Fail(status.code, "listener accept failed");
      if (!accepted) break;
      client.last_receive = now;
      client.last_heartbeat = now;
    }
  }
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (!client.connection.open()) continue;
    if (now < client.last_receive ||
        now - client.last_receive >= s.config.heartbeat_timeout_ns) {
      Disconnect(i);
      continue;
    }
    for (std::size_t j = 0; j < kRequestsPerClient; ++j) {
      Message message;
      const auto result = client.connection.Poll(message);
      if (result.state == ReceiveState::kIdle) break;
      if (result.state != ReceiveState::kMessage) {
        Disconnect(i);
        break;
      }
      client.last_receive = now;
      const auto status = Handle(i, message, now);
      if (!status.ok()) return status;
      if (!client.connection.open()) break;
    }
    if (client.connection.open() && client.authenticated &&
        now - client.last_heartbeat >= kHeartbeatIntervalNs) {
      Message heartbeat;
      heartbeat.type = MessageType::kHeartbeat;
      heartbeat.generation = s.generation;
      if (!client.connection.Queue(heartbeat).ok()) Disconnect(i);
      client.last_heartbeat = now;
    }
  }
  ScheduleClientCancels(now);
  auto status = s.trading.ProcessRequests(s.adapter, now);
  if (!status.ok()) return Fail(status.code, s.trading.error_detail());
  status = Dispatch(now);
  if (!status.ok()) return status;
  if (s.account_dirty) {
    BroadcastAccount();
    s.account_dirty = false;
  }
  NotifyDrained();
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (client.connection.open() && !client.connection.Service().ok())
      Disconnect(i);
    if (client.connection.open() && client.stopping && !client.drained_sent &&
        now >= client.stop_time &&
        now - client.stop_time >= s.config.shutdown_timeout_ns)
      Disconnect(i);
  }
  if (s.stopping && !is_drained() && now >= s.stop_time &&
      now - s.stop_time >= s.config.shutdown_timeout_ns)
    return Fail(base::ErrorCode::kInvalidState,
                "shutdown timed out with unconfirmed orders or replies");
  return {};
}

void Server::BeginStopping(std::uint64_t now) noexcept {
  if (!storage_ || storage_->stopping) return;
  auto& s = *storage_;
  s.stopping = true;
  s.stop_time = now;
  s.listener.Close();
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (!client.authenticated) {
      Disconnect(i);
      continue;
    }
    client.stopping = true;
    client.stop_time = now;
  }
}

bool Server::is_drained() const noexcept {
  if (!storage_) return true;
  const auto& s = *storage_;
  if (!s.trading.is_drained() || !s.adapter.is_drained()) return false;
  for (const auto& client : s.clients) {
    if (!client.authenticated || !client.connection.open()) continue;
    if (!client.connection.empty()) return false;
    if (client.requested_stop) {
      if (!client.drained_sent) return false;
    } else if (!s.stopping) {
      return false;
    }
  }
  return true;
}

std::uint16_t Server::port() const noexcept {
  return storage_ ? storage_->listener.port() : 0;
}

const AccountView& Server::account() const noexcept {
  static const AccountView kEmpty;
  return storage_ ? storage_->account : kEmpty;
}

void Server::ReportUnconfirmed() const noexcept {
  if (!storage_) return;
  const auto& s = *storage_;
  for (std::size_t i = 0; i < s.trading.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!s.trading.TryGetOrder({i + 1}, order) ||
        (model::IsTerminalOrderState(order.state) && !order.cancel_pending))
      continue;
    const auto owner = s.owners[i];
    std::fprintf(
        stderr,
        "yquant-tradingd: unconfirmed client_id=%llu local_order_id=%zu "
        "core_order_id=%llu state=%u filled=%lld leaves=%lld "
        "cancel_pending=%u core_cancel_id=%llu\n",
        static_cast<unsigned long long>(s.clients[owner.client].id),
        owner.local + 1,
        static_cast<unsigned long long>(order.client_order_id.value),
        static_cast<unsigned>(order.state),
        static_cast<long long>(order.cumulative_filled_qty),
        static_cast<long long>(order.leaves_qty),
        static_cast<unsigned>(order.cancel_pending),
        static_cast<unsigned long long>(order.cancel_request_id.value));
  }
}

void Server::Close() noexcept {
  if (!storage_) return;
  (void)storage_->adapter.Stop();
  storage_.reset();
}

}  // namespace yquant::tradingd
