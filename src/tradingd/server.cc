#include "tradingd/server.h"

#include <sys/random.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "tradingd/journal.h"
#include "tradingd/server_storage.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_engine.h"
#include "yquant/trading/trading_persistence.h"

namespace yquant::tradingd {
namespace {

inline constexpr std::size_t kRequestsPerClient = 4;
inline constexpr std::size_t kBrokerEventsPerService = 64;
inline constexpr std::size_t kCancelScanPerService = 64;
inline constexpr std::uint64_t kCancelRetryNs = 10000000;

// Equality uses all token bytes to avoid a prefix-length timing comparison.
bool SameToken(const char* left, const char* right) noexcept {
  unsigned mismatch = 0;
  for (std::size_t i = 0; i <= trading::kMaxTokenBytes; ++i)
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
                   const trading::TradingEngine& trading) noexcept {
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

Server::Server(const base::MonotonicClock& clock) noexcept : clock_(clock) {}
Server::~Server() noexcept { Close(); }

base::Status Server::Fail(base::ErrorCode code, const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

base::Status Server::Init(const ServerConfig& config) noexcept {
  Close();
  error_detail_[0] = '\0';
  base::ConfigError error;
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
  status = s.instruments.Configure(config.market_data);
  if (status.ok()) status = s.trading.Init(config.trading, config.market_data);
  if (status.ok()) status = s.StartPersistence();
  if (status.ok()) status = s.listener.Open(config.listen);
  if (!status.ok()) {
    char detail[256];
    std::snprintf(
        detail, sizeof(detail),
        "cannot initialize account/state/listener: %s%s",
        s.persistence_detail[0] != '\0'       ? s.persistence_detail
        : s.trading.error_detail()[0] != '\0' ? s.trading.error_detail()
                                              : s.journal.error_detail(),
        status.system_error ? status.system_error.message().c_str() : "");
    Close();
    return Fail(status.code, detail);
  }
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
  s.account.type = s.config.trading.initial_account.type;
  s.account.broker_managed = s.trading.broker_managed();
  if (s.account.broker_managed) s.account.type = model::AccountType::kFutures;
  s.account.futures_position_count = 0;
  if (s.account.type == model::AccountType::kFutures) {
    (void)s.trading.TryGetFuturesAccount(s.account.futures);
    for (std::size_t i = 0; i < s.config.market_data.instrument_count; ++i) {
      model::FuturesPositionSnapshot p;
      if (s.trading.TryGetFuturesPosition(
              s.config.market_data.instruments[i].instrument.id, p))
        s.account.futures_positions[s.account.futures_position_count++] = p;
    }
    return;
  }
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
  trading::Message message;
  message.type = trading::MessageType::kAccount;
  message.generation = s.generation;
  message.account = s.account;
  for (std::size_t i = 0; i < s.config.max_clients; ++i) {
    auto& client = s.clients[i];
    if (client.authenticated && !client.drained_sent &&
        client.connection.open() && !client.connection.Queue(message).ok())
      Disconnect(i);
  }
}

base::Status Server::Handle(std::size_t index, const trading::Message& message,
                            std::uint64_t now) noexcept {
  auto& s = *storage_;
  auto& client = s.clients[index];
  if (!client.authenticated) {
    if (message.type != trading::MessageType::kHello ||
        message.generation != 0 || message.client_id == 0 ||
        !SameToken(message.token, s.config.token) ||
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
      if (!s.instruments.TryGetInstrument(message.instruments[i].id,
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
    const auto registered = s.RegisterClient(index);
    if (!registered.ok()) {
      Disconnect(index);
      return {};
    }
    trading::Message welcome;
    welcome.type = trading::MessageType::kWelcome;
    welcome.generation = s.generation;
    welcome.instruments = client.instruments;
    welcome.instrument_count = client.instrument_count;
    welcome.account = s.account;
    if (!client.connection.Queue(welcome).ok()) Disconnect(index);
    return {};
  }
  if (message.generation != s.generation ||
      (message.type != trading::MessageType::kNewOrder &&
       message.type != trading::MessageType::kCancelOrder &&
       message.type != trading::MessageType::kStop &&
       message.type != trading::MessageType::kHeartbeat &&
       message.type != trading::MessageType::kSimulationQuote)) {
    Disconnect(index);
    return {};
  }
  if (message.type == trading::MessageType::kSimulationQuote) {
    if (s.config.trading.adapter !=
            trading::TradingAdapterKind::kSimulatedFutures ||
        client.stopping || s.stopping ||
        !HasInstrument(client, message.quote.instrument_id) ||
        (s.simulation_client_id && s.simulation_client_id != client.id)) {
      Disconnect(index);
      return {};
    }
    const auto slot = s.instruments.FindIndex(message.quote.instrument_id);
    if (!slot) {
      Disconnect(index);
      return {};
    }
    const auto& definition = s.instruments[*slot];
    if (definition.type != model::InstrumentType::kFutures ||
        message.quote.source_id != s.instruments.source_id() ||
        message.quote.bid_price % definition.price_tick ||
        message.quote.ask_price % definition.price_tick ||
        message.quote.bid_qty % definition.quantity_step ||
        message.quote.ask_qty % definition.quantity_step) {
      Disconnect(index);
      return {};
    }
    if (message.quote.sequence <= s.simulation_quotes[*slot].sequence)
      return {};
    s.simulation_client_id = client.id;
    auto quote = message.quote;
    quote.receive_time_ns = now;
    auto status = s.futures.OnQuote(quote, now);
    if (!status.ok()) return Fail(status.code, s.futures.error_detail());
    const auto code = s.trading.ApplyQuote(quote);
    if (code != base::ErrorCode::kOk)
      return Fail(code, s.trading.error_detail());
    s.simulation_quotes[*slot] = quote;
    ++s.account.revision;
    RefreshAccount();
    s.account_dirty = true;
    return {};
  }
  if (message.type == trading::MessageType::kHeartbeat) return {};
  if (message.type == trading::MessageType::kStop) {
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
  if (message.type == trading::MessageType::kNewOrder) {
    if (client.stopping || s.stopping || s.degraded ||
        message.order_id.value != client.order_count + 1 ||
        client.order_count == client.orders.size() ||
        !HasInstrument(client, message.request.instrument_id)) {
      Disconnect(index);
      return {};
    }
    const auto local_index = client.order_count++;
    auto& local = client.orders[local_index];
    s.context_client = index;
    s.context_local = local_index;
    s.context_cancel = {};
    s.trading.SetAcceptingRequests(true);
    const auto code = s.trading.SubmitOrder(message.request, local.core_id);
    s.trading.SetAcceptingRequests(false);
    s.context_client = trading::kMaxClients;
    if (code != base::ErrorCode::kOk && s.degraded) {
      --client.order_count;
      local = {};
      Disconnect(index);
      return {};
    }
    if (code == base::ErrorCode::kOk) {
      s.BindOwner(s.trading.order_count() - 1,
                  {local.core_id, index, local_index});
      ++s.account.revision;
      RefreshAccount();
      s.account_dirty = true;
    } else {
      trading::Message reply;
      reply.type = trading::MessageType::kOrderUpdate;
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
      order.position_effect = message.request.position_effect;
      order.state = model::OrderState::kRejected;
      order.report_sequence = ++local.report_sequence;
      order.receive_time_ns = now;
      local.rejected = order;
      const auto recorded = s.PersistRejection(index, local_index, reply);
      if (!recorded.ok()) return recorded;
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
    s.trading.SetAcceptingRequests(true);
    s.context_client = index;
    s.context_local = message.order_id.value - 1;
    s.context_cancel = message.cancel_id;
    code = s.trading.CancelOrder(local.core_id);
    s.context_client = trading::kMaxClients;
    s.trading.SetAcceptingRequests(false);
  }
  if (code == base::ErrorCode::kOk) {
    (void)s.trading.TryGetOrder(local.core_id, snapshot);
    local.core_cancel = snapshot.cancel_request_id;
    local.local_cancel = message.cancel_id;
  } else {
    // A failed cancel is an explicit result, including after a fill. It never
    // releases reservations or invents a successful broker cancellation.
    trading::Message reply;
    reply.type = trading::MessageType::kOrderUpdate;
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
    const auto recorded =
        s.PersistRejection(index, message.order_id.value - 1, reply);
    if (!recorded.ok()) return recorded;
    if (!client.connection.Queue(reply).ok()) Disconnect(index);
  }
  return {};
}

base::Status Server::Dispatch(std::uint64_t now) noexcept {
  auto& s = *storage_;
  for (std::size_t count = 0; count < kBrokerEventsPerService; ++count) {
    trading::TradingDispatch dispatch;
    bool event = false;
    const auto polled = s.trading.Poll(now, dispatch, event);
    if (!polled.ok()) return Fail(polled.code, s.trading.error_detail());
    if (!event) break;
    if (dispatch.update.order.client_order_id.value == 0) {
      if (s.trading.broker_managed()) {
        ++s.account.revision;
        RefreshAccount();
        s.account_dirty = true;
      }
      continue;
    }
    const auto core_id = dispatch.update.order.client_order_id.value;
    const auto owner_index = s.OwnerIndex({core_id});
    if (owner_index == s.owners.size())
      return Fail(base::ErrorCode::kCorrupt, "missing order ownership");
    const auto owner = s.owners[owner_index];
    auto& client = s.clients[owner.client];
    auto& local = client.orders[owner.local];
    ++s.account.revision;
    RefreshAccount();
    trading::Message message;
    message.type = trading::MessageType::kOrderUpdate;
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
  // Global stopping closes the engine's request window permanently; its own
  // bounded scan then owns all cancellation attempts and retry scheduling.
  if (s.stopping) return;
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
    const model::ClientOrderId id = owner.core_id;
    if (!s.trading.TryGetOrder(id, order) ||
        order.state != model::OrderState::kWorking || order.cancel_pending)
      continue;
    s.trading.SetAcceptingRequests(true);
    s.context_client = owner.client;
    s.context_local = owner.local;
    s.context_cancel = {};
    const auto code = s.trading.CancelOrder(id);
    s.context_client = trading::kMaxClients;
    s.trading.SetAcceptingRequests(false);
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
    trading::Message message;
    message.type = trading::MessageType::kDrained;
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
  s.PollCheckpoint(now);
  if (s.mysql_archive.disk_full()) s.archive_disk_pressure = true;
  if (s.archive_disk_pressure) {
    BeginStopping(now);
    s.trading.BeginStopping();
  }
  if (s.degraded) {
    BeginStopping(now);
    s.trading.BeginStopping();
  }
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
      trading::Message message;
      const auto result = client.connection.Poll(message);
      if (result.state == trading::ReceiveState::kIdle) break;
      if (result.state != trading::ReceiveState::kMessage) {
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
      trading::Message heartbeat;
      heartbeat.type = trading::MessageType::kHeartbeat;
      heartbeat.generation = s.generation;
      if (!client.connection.Queue(heartbeat).ok()) Disconnect(i);
      client.last_heartbeat = now;
    }
  }
  if (s.degraded) {
    BeginStopping(now);
    s.trading.BeginStopping();
  }
  ScheduleClientCancels(now);
  if (s.stopping) {
    const auto scheduled =
        s.trading.ScheduleStopping(now, kCancelScanPerService);
    if (!scheduled.ok() && !s.degraded)
      return Fail(scheduled.code, s.trading.error_detail());
  }
  auto status = s.trading.ProcessRequests(now);
  if (!status.ok() && !s.degraded)
    return Fail(status.code, s.trading.error_detail());
  if (s.degraded) {
    BeginStopping(now);
    s.trading.BeginStopping();
  }
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
  if (s.config.mysql_archive.enabled && !s.degraded)
    s.mysql_archive.PublishApplied(s.journal.published_lsn());
  if (s.archive_disk_pressure && is_drained())
    return Fail(base::ErrorCode::kResourceExhausted,
                "archive disk reserve exhausted");
  if (s.degraded && is_drained())
    return Fail(s.persistence_failure.code, s.persistence_detail);
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
  s.trading.BeginStopping();
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
  if (!s.trading.is_drained()) return false;
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

const trading::AccountView& Server::account() const noexcept {
  static const trading::AccountView kEmpty;
  return storage_ ? storage_->account : kEmpty;
}

void Server::ReportUnconfirmed() const noexcept {
  if (!storage_) return;
  const auto& s = *storage_;
  for (std::size_t i = 0; i < s.trading.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!s.trading.TryGetOrderAt(i, order) ||
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

base::Status Server::Finish() noexcept {
  if (!storage_)
    return Fail(base::ErrorCode::kInvalidState, "server is not initialized");
  auto& s = *storage_;
  if (!is_drained())
    return Fail(base::ErrorCode::kInvalidState, "orders are not drained");
  if (!s.config.persistence.enabled()) return {};
  s.WaitCheckpoint();
  if (s.degraded || s.checkpoint_failed.load(std::memory_order_acquire))
    return Fail(
        base::ErrorCode::kIoError,
        "persistent state has an unrecorded range or checkpoint failure");
  auto status = s.journal.Flush();
  if (status.ok()) status = s.CaptureCheckpoint(clock_.Now());
  if (status.ok()) status = s.PublishCheckpoint();
  if (!status.ok())
    return Fail(status.code, "final state synchronization failed");
  if (s.config.mysql_archive.enabled) {
    s.mysql_archive.PublishApplied(s.journal.published_lsn());
    status = s.SaveArchiveEnd(s.run_id, s.journal.published_lsn());
    if (!status.ok())
      return Fail(status.code, "archive end synchronization failed");
    s.mysql_archive.Stop(true);
  }
  return {};
}

void Server::Close() noexcept {
  if (!storage_) return;
  (void)storage_->trading.SetMutationSink(nullptr);
  storage_->mock.SetMutationObserver(nullptr, nullptr);
  storage_->mysql_archive.Stop(false);
  storage_->StopCheckpoint();
  // Close the engine while its borrowed adapter and journal are still alive.
  // This also clears its prepared state, making subsequent destruction inert.
  (void)storage_->trading.Stop();
  (void)storage_->journal.Close();
  storage_.reset();
}

}  // namespace yquant::tradingd
