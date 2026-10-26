#include "tradingd/server.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#include "tradingd/journal.h"
#include "yquant/base/format.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"
#include "yquant/trading/trading_engine.h"
#include "yquant/trading/trading_persistence.h"

namespace yquant::tradingd {
namespace {

inline constexpr size_t kRequestsPerClient = 4;
inline constexpr size_t kBrokerEventsPerService = 64;
inline constexpr size_t kCancelScanPerService = 64;
inline constexpr uint64_t kCancelRetryNs = 10000000;

// Equality uses all token bytes to avoid a prefix-length timing comparison.
bool SameToken(const char* left, const char* right) noexcept {
  unsigned mismatch = 0;
  for (size_t i = 0; i <= trading::kMaxTokenBytes; ++i) {
    mismatch |= static_cast<unsigned char>(left[i]) ^
                static_cast<unsigned char>(right[i]);
  }
  return mismatch == 0;
}

}  // namespace

bool Server::HasInstrument(const Client& client,
                           model::InstrumentId id) noexcept {
  for (size_t i = 0; i < client.instrument_count; ++i) {
    if (client.instruments[i].id == id) {
      return true;
    }
  }
  return false;
}

Server::Adapter::Adapter(Server* server) noexcept : server_(server) {}

base::Status Server::Adapter::Prepare(
    const trading::TradingConfig& config) noexcept {
  switch (config.adapter) {
    case trading::TradingAdapterKind::kMock:
      server_->selected_adapter_ = &server_->mock_;
      break;
    case trading::TradingAdapterKind::kSimulatedFutures:
      server_->selected_adapter_ = &server_->futures_;
      break;
    case trading::TradingAdapterKind::kCtp:
      server_->selected_adapter_ = &server_->ctp_;
      break;
  }
  return server_->selected_adapter_->Prepare(config);
}
base::Status Server::Adapter::Start(uint64_t now) noexcept {
  return server_->selected_adapter_->Start(now);
}
void Server::Adapter::BeginStopping() noexcept {
  server_->selected_adapter_->BeginStopping();
}
base::Status Server::Adapter::ReadAccountSnapshot(
    trading::TradingAccountSnapshot& out) noexcept {
  return server_->selected_adapter_->ReadAccountSnapshot(out);
}
base::Status Server::Adapter::ReadBrokerAccountSnapshot(
    trading::BrokerAccountSnapshot& out) noexcept {
  return server_->selected_adapter_->ReadBrokerAccountSnapshot(out);
}
base::Status Server::Adapter::SendCancel(
    model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
    model::CancelRequestId cancel, uint64_t now) noexcept {
  return server_->selected_adapter_->SendCancel(id, adapter_order_id, cancel,
                                                now);
}
trading::TradingPollResult Server::Adapter::Poll(
    uint64_t now, model::TradingEvent& out) noexcept {
  return server_->selected_adapter_->Poll(now, out);
}
std::optional<uint64_t> Server::Adapter::next_deadline() const noexcept {
  return server_->selected_adapter_->next_deadline();
}
bool Server::Adapter::is_drained() const noexcept {
  return server_->selected_adapter_->is_drained();
}
base::Status Server::Adapter::Stop() noexcept {
  return server_->selected_adapter_->Stop();
}
const char* Server::Adapter::error_detail() const noexcept {
  return server_->selected_adapter_->error_detail();
}

Server::Server(const base::MonotonicClock* clock) noexcept
    : clock_(*clock),
      ctp_(&instruments_),
      adapter_(this),
      trading_(clock, &instruments_, &adapter_) {}
bool Server::ClientDrained(const Client& client) const noexcept {
  for (size_t i = 0; i < client.order_count; ++i) {
    if (client.orders[i].core_id.value == 0) {
      continue;
    }
    model::OrderSnapshot snapshot;
    if (!trading_.TryGetOrder(client.orders[i].core_id, snapshot) ||
        !model::IsTerminalOrderState(snapshot.state) ||
        snapshot.cancel_pending) {
      return false;
    }
  }
  return true;
}

Server::~Server() noexcept { Close(); }

base::Status Server::Fail(base::ErrorCode code, const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
  return {code, {}};
}

base::Status Server::Init(const ServerConfig& config) noexcept {
  Close();
  error_detail_[0] = '\0';
  base::ConfigError error;
  auto status = ValidateConfig(config, error);
  if (!status.ok()) {
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}: {}",
                             error.field_path, error.message);
    return status;
  }
  config_ = config;
  const auto token_size = std::strlen(config_.token);
  std::memset(config_.token + token_size + 1, 0,
              sizeof(config_.token) - token_size - 1);
  if (getentropy(&generation_, sizeof(generation_)) != 0 || generation_ == 0) {
    Close();
    return Fail(base::ErrorCode::kIoError, "cannot create server generation");
  }
  status = instruments_.Configure(config.market_data);
  const char* stage_detail = "instrument scope setup failed";
  if (status.ok()) {
    status = trading_.Init(config.trading, config.market_data);
    stage_detail = trading_.error_detail();
  }
  if (status.ok()) {
    status = StartPersistence();
    stage_detail = persistence_detail_[0] != '\0' ? persistence_detail_
                                                  : journal_.error_detail();
  }
  if (status.ok()) {
    status = listener_.Open(config.listen);
    stage_detail = "listener startup failed";
  }
  if (!status.ok()) {
    char detail[256];
    ::yquant::base::FormatTo(
        detail, sizeof(detail),
        "cannot initialize account/state/listener: {}{}{}", stage_detail,
        status.system_error ? ": " : "",
        status.system_error ? status.system_error.message().c_str() : "");
    Close();
    return Fail(status.code, detail);
  }
  account_.revision = 1;
  RefreshAccount();
  initialized_ = true;
  return {};
}

void Server::Disconnect(size_t index) noexcept {
  auto& client = clients_[index];
  client.connection.Close();
  if (client.authenticated) {
    client.stopping = true;
  } else {
    client.id = 0;
  }
}

void Server::RefreshAccount() noexcept {
  account_.type = config_.trading.initial_account.type;
  account_.broker_managed = trading_.broker_managed();
  if (account_.broker_managed) {
    account_.type = model::AccountType::kFutures;
  }
  account_.futures_position_count = 0;
  if (account_.type == model::AccountType::kFutures) {
    (void)trading_.TryGetFuturesAccount(account_.futures);
    for (size_t i = 0; i < config_.market_data.instrument_count; ++i) {
      model::FuturesPositionSnapshot p;
      if (trading_.TryGetFuturesPosition(
              config_.market_data.instruments[i].instrument.id, p)) {
        account_.futures_positions[account_.futures_position_count++] = p;
      }
    }
    return;
  }
  (void)trading_.TryGetCash(account_.cash);
  account_.position_count = 0;
  for (size_t i = 0; i < config_.market_data.instrument_count; ++i) {
    model::PositionSnapshot position;
    const auto id = config_.market_data.instruments[i].instrument.id;
    if (trading_.TryGetPosition(id, position) &&
        (position.total_qty != 0 || position.reserved_qty != 0)) {
      account_.positions[account_.position_count++] = position;
    }
  }
}

void Server::BroadcastAccount() noexcept {
  trading::Message message;
  message.type = trading::MessageType::kAccount;
  message.generation = generation_;
  message.account = account_;
  for (size_t i = 0; i < config_.max_clients; ++i) {
    auto& client = clients_[i];
    if (client.authenticated && !client.drained_sent &&
        client.connection.open() && !client.connection.Queue(message).ok()) {
      Disconnect(i);
    }
  }
}

base::Status Server::HandleHello(size_t index,
                                 const trading::Message& message) noexcept {
  auto& client = clients_[index];
  if (message.type != trading::MessageType::kHello || message.generation != 0 ||
      message.client_id == 0 || !SameToken(message.token, config_.token) ||
      message.instrument_count == 0) {
    Disconnect(index);
    return {};
  }
  for (const auto& other : clients_) {
    if (other.id == message.client_id) {
      Disconnect(index);
      return {};
    }
  }
  for (size_t i = 0; i < message.instrument_count; ++i) {
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(message.instruments[i].id, instrument) ||
        instrument.price_decimals != message.instruments[i].price_decimals) {
      Disconnect(index);
      return {};
    }
  }
  client.id = message.client_id;
  client.authenticated = true;
  client.instruments = message.instruments;
  client.instrument_count = message.instrument_count;
  const auto registered = RegisterClient(index);
  if (!registered.ok()) {
    Disconnect(index);
    return {};
  }
  trading::Message welcome;
  welcome.type = trading::MessageType::kWelcome;
  welcome.generation = generation_;
  welcome.instruments = client.instruments;
  welcome.instrument_count = client.instrument_count;
  welcome.account = account_;
  if (!client.connection.Queue(welcome).ok()) {
    Disconnect(index);
  }
  return {};
}

base::Status Server::HandleSimulationQuote(size_t index,
                                           const trading::Message& message,
                                           uint64_t now) noexcept {
  auto& client = clients_[index];
  if (config_.trading.adapter !=
          trading::TradingAdapterKind::kSimulatedFutures ||
      client.stopping || stopping_ ||
      !HasInstrument(client, message.quote.instrument_id) ||
      (simulation_client_id_ && simulation_client_id_ != client.id)) {
    Disconnect(index);
    return {};
  }
  const auto slot = instruments_.FindIndex(message.quote.instrument_id);
  if (!slot) {
    Disconnect(index);
    return {};
  }
  const auto& definition = instruments_[*slot];
  if (definition.type != model::InstrumentType::kFutures ||
      message.quote.source_id != instruments_.source_id() ||
      message.quote.bid_price % definition.price_tick ||
      message.quote.ask_price % definition.price_tick ||
      message.quote.bid_qty % definition.quantity_step ||
      message.quote.ask_qty % definition.quantity_step) {
    Disconnect(index);
    return {};
  }
  if (message.quote.sequence <= simulation_quotes_[*slot].sequence) {
    return {};
  }
  simulation_client_id_ = client.id;
  auto quote = message.quote;
  quote.receive_time_ns = now;
  auto status = futures_.OnQuote(quote, now);
  if (!status.ok()) {
    return Fail(status.code, futures_.error_detail());
  }
  const auto code = trading_.ApplyQuote(quote);
  if (code != base::ErrorCode::kOk) {
    return Fail(code, trading_.error_detail());
  }
  simulation_quotes_[*slot] = quote;
  ++account_.revision;
  RefreshAccount();
  account_dirty_ = true;
  return {};
}

base::Status Server::HandleNewOrder(size_t index,
                                    const trading::Message& message,
                                    uint64_t now) noexcept {
  auto& client = clients_[index];
  if (client.stopping || stopping_ || degraded_ ||
      message.strategy_order_id.value != client.order_count + 1 ||
      client.order_count == client.orders.size() ||
      !HasInstrument(client, message.request.instrument_id)) {
    Disconnect(index);
    return {};
  }
  const auto local_index = client.order_count++;
  auto& local = client.orders[local_index];
  context_client_ = index;
  context_local_ = local_index;
  context_cancel_ = {};
  trading_.SetAcceptingRequests(true);
  const auto code = trading_.SubmitOrder(message.request, local.core_id);
  trading_.SetAcceptingRequests(false);
  context_client_ = trading::kMaxClients;
  if (code != base::ErrorCode::kOk && degraded_) {
    --client.order_count;
    local = {};
    Disconnect(index);
    return {};
  }
  if (code == base::ErrorCode::kOk) {
    BindOwner(trading_.order_count() - 1, {local.core_id, index, local_index});
    ++account_.revision;
    RefreshAccount();
    account_dirty_ = true;
  } else {
    trading::Message reply;
    reply.type = trading::MessageType::kOrderUpdate;
    reply.generation = generation_;
    reply.account = account_;
    reply.update.kind = model::OrderUpdateKind::kRejected;
    reply.update.reason.Set(static_cast<int32_t>(code),
                            trading_.error_detail());
    auto& order = reply.update.order;
    order.strategy_order_id = message.strategy_order_id;
    order.instrument_id = message.request.instrument_id;
    order.side = message.request.side;
    order.limit_price = message.request.limit_price;
    order.original_qty = message.request.quantity;
    order.position_effect = message.request.position_effect;
    order.state = model::OrderState::kRejected;
    order.report_sequence = ++local.report_sequence;
    order.receive_time_ns = now;
    local.rejected = order;
    const auto recorded = PersistRejection(index, local_index, reply);
    if (!recorded.ok()) {
      return recorded;
    }
    if (!client.connection.Queue(reply).ok()) {
      Disconnect(index);
    }
  }
  return {};
}

base::Status Server::HandleCancelOrder(
    size_t index, const trading::Message& message) noexcept {
  auto& client = clients_[index];
  if (message.strategy_order_id.value == 0 ||
      message.strategy_order_id.value > client.order_count ||
      message.cancel_id.value == 0 ||
      message.cancel_id.value <= client.last_cancel_id) {
    Disconnect(index);
    return {};
  }
  client.last_cancel_id = message.cancel_id.value;
  auto& local = client.orders[message.strategy_order_id.value - 1];
  model::OrderSnapshot snapshot = local.rejected;
  const bool has_core = local.core_id.value != 0;
  if (has_core) {
    (void)trading_.TryGetOrder(local.core_id, snapshot);
  }
  if (snapshot.cancel_pending) {
    Disconnect(index);
    return {};
  }
  base::ErrorCode code = base::ErrorCode::kInvalidState;
  if (has_core && !client.stopping && !stopping_ &&
      (message.adapter_order_id.empty() ||
       message.adapter_order_id == snapshot.adapter_order_id)) {
    trading_.SetAcceptingRequests(true);
    context_client_ = index;
    context_local_ = message.strategy_order_id.value - 1;
    context_cancel_ = message.cancel_id;
    code = trading_.CancelOrder(local.core_id);
    context_client_ = trading::kMaxClients;
    trading_.SetAcceptingRequests(false);
  }
  if (code == base::ErrorCode::kOk) {
    (void)trading_.TryGetOrder(local.core_id, snapshot);
    local.core_cancel = snapshot.cancel_request_id;
    local.local_cancel = message.cancel_id;
  } else {
    // A failed cancel is an explicit result, including after a fill. It never
    // releases reservations or invents a successful broker cancellation.
    trading::Message reply;
    reply.type = trading::MessageType::kOrderUpdate;
    reply.generation = generation_;
    reply.account = account_;
    reply.update.order = snapshot;
    reply.update.order.strategy_order_id = message.strategy_order_id;
    reply.update.order.report_sequence = ++local.report_sequence;
    reply.update.order.cancel_pending = false;
    reply.update.order.cancel_request_id = message.cancel_id;
    reply.update.kind = model::OrderUpdateKind::kCancelRejected;
    reply.update.cancel_request_id = message.cancel_id;
    reply.update.reason.Set(static_cast<int32_t>(code),
                            "order is not eligible for this cancel request");
    local.core_cancel = snapshot.cancel_request_id;
    local.local_cancel = message.cancel_id;
    if (!has_core) {
      local.rejected = reply.update.order;
    }
    const auto recorded =
        PersistRejection(index, message.strategy_order_id.value - 1, reply);
    if (!recorded.ok()) {
      return recorded;
    }
    if (!client.connection.Queue(reply).ok()) {
      Disconnect(index);
    }
  }
  return {};
}

base::Status Server::Handle(size_t index, const trading::Message& message,
                            uint64_t now) noexcept {
  auto& client = clients_[index];
  if (!client.authenticated) {
    return HandleHello(index, message);
  }
  if (message.generation != generation_) {
    Disconnect(index);
    return {};
  }
  switch (message.type) {
    case trading::MessageType::kSimulationQuote:
      return HandleSimulationQuote(index, message, now);
    case trading::MessageType::kHeartbeat:
      return {};
    case trading::MessageType::kStop:
      client.requested_stop = true;
      if (!client.stopping) {
        client.stopping = true;
        client.stop_time = now;
      }
      return {};
    case trading::MessageType::kNewOrder:
    case trading::MessageType::kCancelOrder:
      if (!client.connection.can_queue()) {
        Disconnect(index);
        return {};
      }
      return message.type == trading::MessageType::kNewOrder
                 ? HandleNewOrder(index, message, now)
                 : HandleCancelOrder(index, message);
    case trading::MessageType::kHello:
    case trading::MessageType::kWelcome:
    case trading::MessageType::kOrderUpdate:
    case trading::MessageType::kAccount:
    case trading::MessageType::kDrained:
    case trading::MessageType::kError:
      Disconnect(index);
      return {};
  }
  Disconnect(index);
  return {};
}

base::Status Server::Dispatch(uint64_t now) noexcept {
  for (size_t count = 0; count < kBrokerEventsPerService; ++count) {
    trading::TradingDispatch dispatch;
    bool event = false;
    const auto polled = trading_.Poll(now, dispatch, event);
    if (!polled.ok()) {
      return Fail(polled.code, trading_.error_detail());
    }
    if (!event) {
      break;
    }
    if (dispatch.update.order.client_order_id.value == 0) {
      if (trading_.broker_managed()) {
        ++account_.revision;
        RefreshAccount();
        account_dirty_ = true;
      }
      continue;
    }
    const auto core_id = dispatch.update.order.client_order_id.value;
    const auto owner_index = OwnerIndex({core_id});
    if (owner_index == owners_.size()) {
      return Fail(base::ErrorCode::kCorrupt, "missing order ownership");
    }
    const auto owner = owners_[owner_index];
    auto& client = clients_[owner.client];
    auto& local = client.orders[owner.local];
    ++account_.revision;
    RefreshAccount();
    trading::Message message;
    message.type = trading::MessageType::kOrderUpdate;
    message.generation = generation_;
    message.notify_order = dispatch.notify_order;
    message.has_fill = dispatch.has_fill;
    message.update = dispatch.update;
    message.fill = dispatch.fill;
    message.account = account_;
    message.update.order.strategy_order_id.value = owner.local + 1;
    message.update.order.report_sequence = ++local.report_sequence;
    if (dispatch.has_fill) {
      message.fill.strategy_order_id.value = owner.local + 1;
    }
    const auto wire_cancel =
        message.update.order.cancel_request_id == local.core_cancel
            ? local.local_cancel
            : model::CancelRequestId{};
    message.update.order.cancel_request_id = wire_cancel;
    const bool cancel_report =
        dispatch.update.kind == model::OrderUpdateKind::kCanceled ||
        dispatch.update.kind == model::OrderUpdateKind::kCancelRejected ||
        dispatch.update.kind == model::OrderUpdateKind::kCancelNotSent;
    message.update.cancel_request_id =
        cancel_report && dispatch.update.cancel_request_id == local.core_cancel
            ? local.local_cancel
            : model::CancelRequestId{};
    if (client.connection.open() && !client.connection.Queue(message).ok()) {
      Disconnect(owner.client);
    }
    if (client.connection.open() && !client.connection.Service().ok()) {
      Disconnect(owner.client);
    }
    account_dirty_ = true;
    if (dispatch.update.kind == model::OrderUpdateKind::kCancelRejected) {
      local.retry_at =
          now > UINT64_MAX - kCancelRetryNs ? UINT64_MAX : now + kCancelRetryNs;
    }
  }
  return {};
}

void Server::ScheduleClientCancels(uint64_t now) noexcept {
  // Global stopping closes the engine's request window permanently; its own
  // bounded scan then owns all cancellation attempts and retry scheduling.
  if (stopping_) {
    return;
  }
  const auto count = trading_.order_count();
  for (size_t scanned = 0; scanned < kCancelScanPerService && count != 0;
       ++scanned) {
    if (cancel_cursor_ >= count) {
      cancel_cursor_ = 0;
    }
    const auto core_index = cancel_cursor_++;
    const auto owner = owners_[core_index];
    auto& client = clients_[owner.client];
    auto& local = client.orders[owner.local];
    if (!client.stopping || local.retry_at > now) {
      continue;
    }
    model::OrderSnapshot order;
    const model::ClientOrderId id = owner.core_id;
    if (!trading_.TryGetOrder(id, order) ||
        order.state != model::OrderState::kWorking || order.cancel_pending) {
      continue;
    }
    trading_.SetAcceptingRequests(true);
    context_client_ = owner.client;
    context_local_ = owner.local;
    context_cancel_ = {};
    const auto code = trading_.CancelOrder(id);
    context_client_ = trading::kMaxClients;
    trading_.SetAcceptingRequests(false);
    if (code == base::ErrorCode::kOk) {
      (void)trading_.TryGetOrder(id, order);
      local.core_cancel = order.cancel_request_id;
      local.local_cancel = {};
    }
  }
}

void Server::NotifyDrained() noexcept {
  for (size_t i = 0; i < config_.max_clients; ++i) {
    auto& client = clients_[i];
    if (!client.authenticated || !client.connection.open() ||
        !client.requested_stop || client.drained_sent ||
        !ClientDrained(client)) {
      continue;
    }
    trading::Message message;
    message.type = trading::MessageType::kDrained;
    message.generation = generation_;
    if (!client.connection.Queue(message).ok()) {
      Disconnect(i);
    } else {
      client.drained_sent = true;
    }
  }
}

base::Status Server::AcceptClients(uint64_t now) noexcept {
  if (stopping_) {
    return {};
  }
  for (size_t i = 0; i < config_.max_clients; ++i) {
    auto& client = clients_[i];
    if (client.id != 0 || client.connection.open()) {
      continue;
    }
    bool accepted = false;
    const auto status = listener_.Accept(client.connection, accepted);
    if (!status.ok()) {
      return Fail(status.code, "listener accept failed");
    }
    if (!accepted) {
      break;
    }
    client.last_receive = now;
    client.last_heartbeat = now;
  }
  return {};
}

base::Status Server::ServiceClient(size_t index, uint64_t now) noexcept {
  auto& client = clients_[index];
  if (!client.connection.open()) {
    return {};
  }
  if (now < client.last_receive ||
      now - client.last_receive >= config_.heartbeat_timeout_ns) {
    Disconnect(index);
    return {};
  }
  for (size_t j = 0; j < kRequestsPerClient; ++j) {
    trading::Message message;
    const auto result = client.connection.Poll(message);
    if (result.state == trading::ReceiveState::kIdle) {
      break;
    }
    if (result.state != trading::ReceiveState::kMessage) {
      Disconnect(index);
      break;
    }
    client.last_receive = now;
    const auto status = Handle(index, message, now);
    if (!status.ok()) {
      return status;
    }
    if (!client.connection.open()) {
      break;
    }
  }
  if (client.connection.open() && client.authenticated &&
      now - client.last_heartbeat >= kHeartbeatIntervalNs) {
    trading::Message heartbeat;
    heartbeat.type = trading::MessageType::kHeartbeat;
    heartbeat.generation = generation_;
    if (!client.connection.Queue(heartbeat).ok()) {
      Disconnect(index);
    }
    client.last_heartbeat = now;
  }
  return {};
}

void Server::FlushClients(uint64_t now) noexcept {
  for (size_t i = 0; i < config_.max_clients; ++i) {
    auto& client = clients_[i];
    if (client.connection.open() && !client.connection.Service().ok()) {
      Disconnect(i);
    }
    if (client.connection.open() && client.stopping && !client.drained_sent &&
        now >= client.stop_time &&
        now - client.stop_time >= config_.shutdown_timeout_ns) {
      Disconnect(i);
    }
  }
}

base::Status Server::Service(uint64_t now) noexcept {
  if (!initialized_) {
    return Fail(base::ErrorCode::kInvalidState, "server is not initialized");
  }
  PollCheckpoint(now);
  if (mysql_archive_.disk_full()) {
    archive_disk_pressure_ = true;
  }
  if (archive_disk_pressure_) {
    BeginStopping(now);
    trading_.BeginStopping();
  }
  if (degraded_) {
    BeginStopping(now);
    trading_.BeginStopping();
  }
  auto status = AcceptClients(now);
  if (!status.ok()) {
    return status;
  }
  for (size_t i = 0; i < config_.max_clients; ++i) {
    status = ServiceClient(i, now);
    if (!status.ok()) {
      return status;
    }
  }
  if (degraded_) {
    BeginStopping(now);
    trading_.BeginStopping();
  }
  ScheduleClientCancels(now);
  if (stopping_) {
    const auto scheduled =
        trading_.ScheduleStopping(now, kCancelScanPerService);
    if (!scheduled.ok() && !degraded_) {
      return Fail(scheduled.code, trading_.error_detail());
    }
  }
  status = trading_.ProcessRequests(now);
  if (!status.ok() && !degraded_) {
    return Fail(status.code, trading_.error_detail());
  }
  if (degraded_) {
    BeginStopping(now);
    trading_.BeginStopping();
  }
  status = Dispatch(now);
  if (!status.ok()) {
    return status;
  }
  if (account_dirty_) {
    BroadcastAccount();
    account_dirty_ = false;
  }
  NotifyDrained();
  FlushClients(now);
  if (config_.mysql_archive.enabled && !degraded_) {
    mysql_archive_.PublishApplied(journal_.published_lsn());
  }
  if (archive_disk_pressure_ && is_drained()) {
    return Fail(base::ErrorCode::kResourceExhausted,
                "archive disk reserve exhausted");
  }
  if (degraded_ && is_drained()) {
    return Fail(persistence_failure_.code, persistence_detail_);
  }
  if (stopping_ && !is_drained() && now >= stop_time_ &&
      now - stop_time_ >= config_.shutdown_timeout_ns) {
    return Fail(base::ErrorCode::kInvalidState,
                "shutdown timed out with unconfirmed orders or replies");
  }
  return {};
}

void Server::BeginStopping(uint64_t now) noexcept {
  if (!initialized_ || stopping_) {
    return;
  }
  stopping_ = true;
  stop_time_ = now;
  trading_.BeginStopping();
  listener_.Close();
  for (size_t i = 0; i < config_.max_clients; ++i) {
    auto& client = clients_[i];
    if (!client.authenticated) {
      Disconnect(i);
      continue;
    }
    client.stopping = true;
    client.stop_time = now;
  }
}

bool Server::is_drained() const noexcept {
  if (!initialized_) {
    return true;
  }
  if (!trading_.is_drained()) {
    return false;
  }
  for (const auto& client : clients_) {
    if (!client.authenticated || !client.connection.open()) {
      continue;
    }
    if (!client.connection.empty()) {
      return false;
    }
    if (client.requested_stop) {
      if (!client.drained_sent) {
        return false;
      }
    } else if (!stopping_) {
      return false;
    }
  }
  return true;
}

uint16_t Server::port() const noexcept {
  return initialized_ ? listener_.port() : 0;
}

const trading::AccountView& Server::account() const noexcept {
  static const trading::AccountView kEmpty;
  return initialized_ ? account_ : kEmpty;
}

void Server::ReportUnconfirmed() const noexcept {
  if (!initialized_) {
    return;
  }
  for (size_t i = 0; i < trading_.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading_.TryGetOrderAt(i, order) ||
        (model::IsTerminalOrderState(order.state) && !order.cancel_pending)) {
      continue;
    }
    const auto owner = owners_[i];
    ::yquant::base::Print(
        stderr,
        "yquant-tradingd: unconfirmed client_id={} local_order_id={} "
        "core_order_id={} state={} filled={} leaves={} cancel_pending={} "
        "core_cancel_id={}\n",
        clients_[owner.client].id, owner.local + 1, order.client_order_id.value,
        static_cast<unsigned>(order.state), order.cumulative_filled_qty,
        order.leaves_qty, static_cast<unsigned>(order.cancel_pending),
        order.cancel_request_id.value);
  }
}

base::Status Server::Finish() noexcept {
  if (!initialized_) {
    return Fail(base::ErrorCode::kInvalidState, "server is not initialized");
  }
  if (!is_drained()) {
    return Fail(base::ErrorCode::kInvalidState, "orders are not drained");
  }
  if (!config_.persistence.enabled()) {
    return {};
  }
  WaitCheckpoint();
  if (degraded_ || checkpoint_failed_.load(std::memory_order_acquire)) {
    return Fail(
        base::ErrorCode::kIoError,
        "persistent state has an unrecorded range or checkpoint failure");
  }
  auto status = journal_.Flush();
  if (status.ok()) {
    status = CaptureCheckpoint(clock_.Now());
  }
  if (status.ok()) {
    status = PublishCheckpoint();
  }
  if (!status.ok()) {
    return Fail(status.code, "final state synchronization failed");
  }
  if (config_.mysql_archive.enabled) {
    mysql_archive_.PublishApplied(journal_.published_lsn());
    status = SaveArchiveEnd(run_id_, journal_.published_lsn());
    if (!status.ok()) {
      return Fail(status.code, "archive end synchronization failed");
    }
    mysql_archive_.Stop(true);
  }
  return {};
}

void Server::Close() noexcept {
  trading_.SetAcceptingRequests(false);
  (void)trading_.SetMutationSink(nullptr);
  mock_.SetMutationObserver(nullptr, nullptr);
  mysql_archive_.Stop(false);
  StopCheckpoint();
  trading_.Reset();
  mock_.Reset();
  futures_.Reset();
  (void)ctp_.Stop();
  (void)journal_.Close();
  listener_.Close();
  for (auto& client : clients_) {
    client.Reset();
  }
  instruments_.Reset();
  ResetRuntimeState();
}

void Server::Client::Reset() noexcept {
  connection.Close();
  id = 0;
  session_id = 0;
  authenticated = false;
  stopping = false;
  requested_stop = false;
  drained_sent = false;
  last_receive = 0;
  last_heartbeat = 0;
  stop_time = 0;
  instruments.fill({});
  instrument_count = 0;
  orders.fill({});
  order_count = 0;
  last_cancel_id = 0;
}

void Server::ResetRuntimeState() noexcept {
  initialized_ = false;
  config_ = {};
  selected_adapter_ = &mock_;
  simulation_client_id_ = 0;
  simulation_quotes_.fill({});
  owners_.fill({});
  owner_lookup_.fill(0);
  account_ = {};
  generation_ = 0;
  stopping_ = false;
  stop_time_ = 0;
  cancel_cursor_ = 0;
  account_dirty_ = false;
  ResetPersistenceState();
}

}  // namespace yquant::tradingd
