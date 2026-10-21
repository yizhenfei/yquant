#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <new>
#include <thread>

#include "tradingd/recovery.h"
#include "tradingd/server_storage.h"
#include "tradingd/state_codec.h"

namespace yquant::tradingd {
namespace {
inline constexpr std::uint64_t kGatewayStateVersion = 1;
inline constexpr std::uint16_t kLedgerRecord = 1;
inline constexpr std::uint16_t kMockRecord = 2;
inline constexpr std::uint16_t kRejectedRecord = 3;
inline constexpr std::size_t kIdentityBufferBytes = 32 * 1024;
inline constexpr std::size_t kControlBufferBytes = 256;
inline constexpr std::size_t kSnapshotNameBytes = 96;
inline constexpr std::size_t kRecoveryStepBudget =
    32 * strategyd::kMaxMockTradingReports;

base::Status CorruptState() noexcept { return {base::ErrorCode::kCorrupt, {}}; }

// The wire codec already validates all order/account fields and avoids native
// layouts. Token-bearing Hello messages are never used in persistent records.
base::Status WriteMessage(StateWriter& writer,
                          const Message& message) noexcept {
  std::array<std::byte, kMaxFrameSize> bytes{};
  std::size_t size = 0;
  auto status = EncodeMessage(message, bytes, size);
  if (!status.ok()) return status;
  writer.U64(size);
  writer.Bytes(std::span(bytes).first(size));
  return writer.ok() ? base::Status{}
                     : base::Status{base::ErrorCode::kResourceExhausted, {}};
}
base::Status ReadMessage(StateReader& reader, Message& message) noexcept {
  const auto size = reader.U64();
  if (!reader.ok() || size > kMaxFrameSize) return CorruptState();
  const auto bytes = reader.Bytes(size);
  if (!reader.ok()) return CorruptState();
  return DecodeMessage(bytes, message);
}
}  // namespace

namespace {
// Fixed half-full table keeps external stable IDs independent of slot indices.
std::size_t OwnerBucket(std::uint64_t id) noexcept {
  id = (id ^ (id >> 30)) * 0xbf58476d1ce4e5b9ULL;
  id = (id ^ (id >> 27)) * 0x94d049bb133111ebULL;
  return (id ^ (id >> 31)) & (kOwnerLookupCapacity - 1);
}
}  // namespace
std::size_t Server::Storage::OwnerIndex(
    model::ClientOrderId id) const noexcept {
  auto bucket = OwnerBucket(id.value);
  for (std::size_t i = 0; i < owner_lookup.size(); ++i) {
    const auto value = owner_lookup[bucket];
    if (value == 0) return owners.size();
    if (owners[value - 1].core_id == id) return value - 1;
    bucket = (bucket + 1) & (kOwnerLookupCapacity - 1);
  }
  return owners.size();
}
void Server::Storage::BindOwner(std::size_t slot,
                                const OrderOwner& owner) noexcept {
  owners[slot] = owner;
  auto bucket = OwnerBucket(owner.core_id.value);
  while (owner_lookup[bucket] != 0 && owner_lookup[bucket] != slot + 1)
    bucket = (bucket + 1) & (kOwnerLookupCapacity - 1);
  owner_lookup[bucket] = static_cast<std::uint16_t>(slot + 1);
}

base::Status Server::Storage::Adapter::SendOrder(
    model::ClientOrderId id, const model::OrderRequest& request,
    std::uint64_t now) noexcept {
  if (!storage.config.persistence.enabled())
    return storage.mock.SendOrder(id, request, now);
  const auto index = storage.OwnerIndex(id);
  if (index == storage.owners.size()) return CorruptState();
  const auto owner = storage.owners[index];
  char key[kExternalRequestKeyBytes + 1];
  const auto status = EncodeExternalRequestKey(
      storage.clients[owner.client].session_id, ExternalRequestKind::kNewOrder,
      owner.local + 1, key);
  if (!status.ok()) return status;
  return storage.mock.SendOrderWithIdentity(id, request, key, now);
}

base::Status Server::Storage::Append(std::uint16_t type,
                                     std::span<const std::byte> payload,
                                     bool new_risk) noexcept {
  if (replaying || !config.persistence.enabled()) return {};
  base::Status status = persistence_failure;
  if (!degraded) {
    std::uint64_t lsn = 0;
    status = journal.TryAppend(type, payload, lsn);
    if (status.ok()) return {};
    degraded = true;
    persistence_failure = status;
    std::snprintf(persistence_detail, sizeof(persistence_detail),
                  "journal append failed: %s", journal.error_detail());
  }
  // True broker facts and risk-reducing cancels still commit in memory. New
  // orders/sends fail before their ledger change or external side effect.
  return new_risk ? status : base::Status{};
}

base::Status Server::Storage::BeforeCommit(
    const strategyd::TradingMutation& mutation) noexcept {
  if (!config.persistence.enabled() || replaying) return {};
  using Kind = strategyd::TradingMutationKind;
  std::size_t client_index = context_client;
  std::size_t local_index = context_local;
  auto cancel = context_cancel;
  if (mutation.kind != Kind::kNewOrder && context_client == kMaxClients) {
    const auto index = OwnerIndex(mutation.order_id);
    if (index == owners.size()) return CorruptState();
    client_index = owners[index].client;
    local_index = owners[index].local;
    cancel = clients[client_index].orders[local_index].local_cancel;
  }
  if (client_index >= config.max_clients ||
      local_index >= strategyd::kMaxTradingOrders)
    return CorruptState();
  std::array<std::byte, kJournalMaxPayloadBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(client_index);
  writer.U64(local_index);
  writer.U64(cancel.value);
  std::array<std::byte, strategyd::kMaxEncodedTradingMutation> fact{};
  std::size_t size = 0;
  auto status = strategyd::EncodeTradingMutation(mutation, fact, size);
  if (!status.ok()) return status;
  writer.Bytes(std::span(fact).first(size));
  status = Append(
      kLedgerRecord, std::span(bytes).first(writer.size()),
      mutation.kind == Kind::kNewOrder || mutation.kind == Kind::kSendNew);
  if (status.ok()) last_record_time = mutation.now;
  return status;
}

base::Status Server::Storage::ObserveMock(
    void* context, const strategyd::MockTradingMutation& mutation) noexcept {
  auto& s = *static_cast<Storage*>(context);
  std::array<std::byte, strategyd::kMaxMockTradingMutationBytes> bytes{};
  std::size_t size = 0;
  auto status = strategyd::EncodeMockTradingMutation(mutation, bytes, size);
  if (!status.ok()) return status;
  status =
      s.Append(kMockRecord, std::span(bytes).first(size),
               mutation.kind == strategyd::MockTradingMutationKind::kSendOrder);
  if (status.ok()) s.last_record_time = mutation.now;
  return status;
}

base::Status Server::Storage::PersistRejection(
    std::size_t client, std::size_t local, const Message& message) noexcept {
  if (!config.persistence.enabled()) return {};
  std::array<std::byte, kJournalMaxPayloadBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(client);
  writer.U64(local);
  auto status = WriteMessage(writer, message);
  if (!status.ok()) return status;
  return Append(kRejectedRecord, std::span(bytes).first(writer.size()), false);
}

base::Status Server::Storage::ReplayRecord(
    void* context, std::uint16_t type, std::uint64_t,
    std::span<const std::byte> payload) noexcept {
  auto& s = *static_cast<Storage*>(context);
  if (type == kMockRecord) {
    strategyd::MockTradingMutation mutation;
    auto status = strategyd::DecodeMockTradingMutation(payload, mutation);
    if (status.ok()) status = s.mock.ReplayMutation(mutation);
    if (status.ok()) s.last_record_time = mutation.now;
    return status;
  }
  StateReader reader(payload);
  const auto client_index = reader.U64();
  const auto local_index = reader.U64();
  if (!reader.ok() || client_index >= s.config.max_clients ||
      local_index >= strategyd::kMaxTradingOrders ||
      s.clients[client_index].id == 0)
    return CorruptState();
  auto& client = s.clients[client_index];
  auto& local = client.orders[local_index];
  client.order_count =
      std::max(client.order_count, static_cast<std::size_t>(local_index + 1));
  if (type == kRejectedRecord) {
    Message message;
    auto status = ReadMessage(reader, message);
    if (!status.ok() || !reader.done() ||
        message.type != MessageType::kOrderUpdate ||
        message.update.order.client_order_id.value != local_index + 1)
      return CorruptState();
    if (local.core_id.value == 0) local.rejected = message.update.order;
    local.report_sequence = message.update.order.report_sequence;
    client.last_cancel_id =
        std::max(client.last_cancel_id, message.update.cancel_request_id.value);
    local.local_cancel = message.update.cancel_request_id;
    return {};
  }
  if (type != kLedgerRecord) return CorruptState();
  const auto local_cancel = reader.U64();
  strategyd::TradingMutation mutation;
  const auto encoded = reader.Bytes(payload.size() - 24);
  if (!reader.done()) return CorruptState();
  auto status = strategyd::DecodeTradingMutation(encoded, mutation);
  if (!status.ok()) return status;
  strategyd::TradingDispatch dispatch;
  status = s.trading.ReplayMutation(mutation, &dispatch);
  if (!status.ok()) return status;
  using Kind = strategyd::TradingMutationKind;
  if (mutation.kind == Kind::kNewOrder) {
    const auto index = s.trading.order_count() - 1;
    s.BindOwner(index,
                {mutation.order_id, static_cast<std::size_t>(client_index),
                 static_cast<std::size_t>(local_index)});
    local.core_id = mutation.order_id;
  } else if (mutation.kind == Kind::kCancel) {
    local.core_cancel = mutation.cancel_id;
    local.local_cancel = {local_cancel};
    client.last_cancel_id = std::max(client.last_cancel_id, local_cancel);
  }
  if (dispatch.update.order.client_order_id.value != 0) ++local.report_sequence;
  s.last_record_time = mutation.now;
  return {};
}

base::Status Server::Storage::SaveIdentity() noexcept {
  WaitCheckpoint();
  std::array<std::byte, kIdentityBufferBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(kGatewayStateVersion);
  writer.Text(config.persistence.account_key);
  for (const auto value : ledger_id) writer.U64(value);
  writer.U64(last_namespace);
  writer.U64(order_id_highwater);
  writer.U64(cancel_id_highwater);
  writer.U64(config.market_data.instrument_count);
  for (std::size_t i = 0; i < config.market_data.instrument_count; ++i) {
    const auto& instrument = config.market_data.instruments[i].instrument;
    writer.U64(instrument.id.value);
    writer.U64(instrument.price_decimals);
  }
  writer.U64(static_cast<unsigned>(config.trading.mode));
  writer.U64(config.trading.report_interval_ns);
  for (const auto& client : clients) {
    writer.U64(client.id);
    writer.U64(client.session_id);
    writer.U64(client.instrument_count);
    for (std::size_t j = 0; j < client.instrument_count; ++j) {
      writer.U64(client.instruments[j].id.value);
      writer.U64(client.instruments[j].price_decimals);
    }
  }
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  return journal.SaveFile("IDENTITY", std::span(bytes).first(writer.size()));
}

base::Status Server::Storage::LoadIdentity() noexcept {
  std::array<std::byte, kIdentityBufferBytes> bytes{};
  std::size_t size = 0;
  auto status = journal.LoadFile("IDENTITY", bytes, size);
  if (!status.ok()) return status;
  StateReader reader(std::span(bytes).first(size));
  if (reader.U64() != kGatewayStateVersion) return CorruptState();
  char key[kMaxAccountKeyBytes + 1];
  if (!reader.Text(key, sizeof(key)) ||
      std::strcmp(key, config.persistence.account_key) != 0)
    return CorruptState();
  for (auto& value : ledger_id) value = reader.U64();
  last_namespace = reader.U64();
  order_id_highwater = reader.U64();
  cancel_id_highwater = reader.U64();
  if ((ledger_id[0] | ledger_id[1]) == 0 ||
      reader.U64() != config.market_data.instrument_count)
    return CorruptState();
  for (std::size_t i = 0; i < config.market_data.instrument_count; ++i) {
    const auto& instrument = config.market_data.instruments[i].instrument;
    if (reader.U64() != instrument.id.value ||
        reader.U64() != instrument.price_decimals)
      return CorruptState();
  }
  if (reader.U64() != static_cast<unsigned>(config.trading.mode) ||
      reader.U64() != config.trading.report_interval_ns)
    return CorruptState();
  for (std::size_t i = 0; i < clients.size(); ++i) {
    auto& client = clients[i];
    client.id = reader.U64();
    client.session_id = reader.U64();
    client.instrument_count = reader.U64();
    if (client.instrument_count > kMaxInstruments ||
        ((client.id == 0) != (client.session_id == 0)) ||
        client.session_id > last_namespace ||
        (client.id != 0 && i >= config.max_clients))
      return CorruptState();
    for (std::size_t j = 0; j < client.instrument_count; ++j) {
      const auto id = reader.U64();
      const auto decimals = reader.U64();
      if (decimals > UINT8_MAX) return CorruptState();
      client.instruments[j] = {{id}, static_cast<std::uint8_t>(decimals)};
      model::InstrumentDefinition known;
      if (!market_data.TryGetInstrument(client.instruments[j].id, known) ||
          known.price_decimals != decimals)
        return CorruptState();
    }
    if (client.id != 0) {
      if (client.instrument_count == 0) return CorruptState();
      client.authenticated = true;
      client.stopping = true;
      for (std::size_t j = 0; j < i; ++j)
        if (clients[j].id == client.id ||
            clients[j].session_id == client.session_id)
          return CorruptState();
    }
  }
  return reader.done() ? base::Status{} : CorruptState();
}

base::Status Server::Storage::RegisterClient(std::size_t index) noexcept {
  if (!config.persistence.enabled()) return {};
  if (last_namespace == UINT64_MAX)
    return {base::ErrorCode::kResourceExhausted, {}};
  clients[index].session_id = ++last_namespace;
  auto status = SaveIdentity();
  if (!status.ok()) {
    degraded = true;
    persistence_failure = status;
    std::snprintf(persistence_detail, sizeof(persistence_detail),
                  "client identity sync failed");
  }
  return status;
}

base::Status Server::Storage::CaptureCheckpoint(std::uint64_t now) noexcept {
  if (degraded) return persistence_failure;
  StateWriter writer(std::span(checkpoint.get(), kJournalMaxSnapshotBytes));
  writer.U64(kGatewayStateVersion);
  for (const auto id : ledger_id) writer.U64(id);
  writer.U64(run_id);
  checkpoint_cursor = journal.GetCursor();
  checkpoint_lsn = checkpoint_cursor.lsn;
  writer.U64(checkpoint_lsn);
  writer.U64(checkpoint_cursor.segment);
  writer.U64(checkpoint_cursor.offset);
  writer.U64(now);
  for (const auto& client : clients) {
    writer.U64(client.id);
    writer.U64(client.session_id);
    writer.U64(client.order_count);
    writer.U64(client.last_cancel_id);
    for (std::size_t i = 0; i < client.order_count; ++i) {
      const auto& local = client.orders[i];
      writer.U64(local.core_id.value);
      writer.U64(local.report_sequence);
      writer.U64(local.core_cancel.value);
      writer.U64(local.local_cancel.value);
      writer.U64(local.retry_at);
      if (local.core_id.value == 0) {
        Message message;
        message.type = MessageType::kOrderUpdate;
        message.update.kind = model::OrderUpdateKind::kRejected;
        message.update.order = local.rejected;
        message.account = account;
        auto status = WriteMessage(writer, message);
        if (!status.ok()) return status;
      }
    }
  }
  // Each codec writes directly into the immutable checkpoint buffer. Only its
  // length prefix is patched; there is no temporary full-state copy.
  auto encode_blob = [&](auto encode) noexcept -> base::Status {
    if (writer.tail().size() < sizeof(std::uint64_t))
      return base::Status{base::ErrorCode::kResourceExhausted, {}};
    auto prefix = writer.tail().first(sizeof(std::uint64_t));
    writer.U64(0);
    std::size_t size = 0;
    auto status = encode(writer.tail(), size);
    if (!status.ok()) return status;
    StateWriter length(prefix);
    length.U64(size);
    writer.Advance(size);
    return {};
  };
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  auto status = encode_blob([&](auto out, auto& size) noexcept {
    return trading.ExportCheckpoint(out, size);
  });
  if (status.ok())
    status = encode_blob([&](auto out, auto& size) noexcept {
      return mock.EncodeState(out, size, now);
    });
  if (!status.ok()) return status;
  if (!writer.ok()) return {base::ErrorCode::kResourceExhausted, {}};
  checkpoint_size = writer.size();
  return {};
}

base::Status Server::Storage::PublishCheckpoint() noexcept {
  char name[kSnapshotNameBytes];
  std::snprintf(name, sizeof(name), "snapshot-%llu-%llu.bin",
                static_cast<unsigned long long>(run_id),
                static_cast<unsigned long long>(checkpoint_lsn));
  auto status = journal.SaveFile(name, {checkpoint.get(), checkpoint_size});
  if (!status.ok()) return status;
  std::array<std::byte, kControlBufferBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(kGatewayStateVersion);
  writer.U64(run_id);
  writer.U64(checkpoint_lsn);
  writer.U64(checkpoint_cursor.segment);
  writer.U64(checkpoint_cursor.offset);
  writer.Text(name);
  return journal.SaveFile("CURRENT", std::span(bytes).first(writer.size()));
}

base::Status Server::Storage::LoadCheckpoint(std::uint64_t& stream,
                                             std::uint64_t& covered) noexcept {
  std::array<std::byte, kControlBufferBytes> bytes{};
  std::size_t size = 0;
  auto status = journal.LoadFile("CURRENT", bytes, size);
  if (!status.ok()) return status;
  StateReader control(std::span(bytes).first(size));
  if (control.U64() != kGatewayStateVersion) return CorruptState();
  stream = control.U64();
  covered = control.U64();
  replay_cursor = {stream, control.U64(), control.U64(), covered};
  char name[kSnapshotNameBytes];
  if (!control.Text(name, sizeof(name)) || !control.done() || stream == 0 ||
      stream > last_namespace)
    return CorruptState();
  status = journal.LoadFile(name, {checkpoint.get(), kJournalMaxSnapshotBytes},
                            size);
  if (!status.ok()) return status;
  StateReader reader(std::span(checkpoint.get(), size));
  if (reader.U64() != kGatewayStateVersion) return CorruptState();
  for (const auto id : ledger_id)
    if (reader.U64() != id) return CorruptState();
  if (reader.U64() != stream || reader.U64() != covered ||
      reader.U64() != replay_cursor.segment ||
      reader.U64() != replay_cursor.offset)
    return CorruptState();
  last_record_time = reader.U64();
  for (auto& client : clients) {
    const auto id = reader.U64();
    const auto session = reader.U64();
    if (id != 0 && (id != client.id || session != client.session_id))
      return CorruptState();
    const auto count = reader.U64();
    client.last_cancel_id = reader.U64();
    if (count > client.orders.size() || (id == 0 && count != 0))
      return CorruptState();
    client.order_count = count;
    for (std::size_t i = 0; i < count; ++i) {
      auto& local = client.orders[i];
      local.core_id = {reader.U64()};
      local.report_sequence = reader.U64();
      local.core_cancel = {reader.U64()};
      local.local_cancel = {reader.U64()};
      local.retry_at = reader.U64();
      if (local.core_id.value == 0) {
        Message message;
        status = ReadMessage(reader, message);
        if (!status.ok() || message.type != MessageType::kOrderUpdate ||
            message.update.order.client_order_id.value != i + 1)
          return CorruptState();
        local.rejected = message.update.order;
      }
    }
  }
  const auto ledger_size = reader.U64();
  const auto ledger = reader.Bytes(ledger_size);
  if (!reader.ok()) return CorruptState();
  status = trading.RestoreCheckpoint(ledger);
  if (!status.ok()) return status;
  const auto mock_size = reader.U64();
  const auto mock_state = reader.Bytes(mock_size);
  if (!reader.done()) return CorruptState();
  status = mock.RestoreState(mock_state, last_record_time);
  if (!status.ok()) return status;
  for (std::size_t i = 0; i < trading.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading.TryGetOrderAt(i, order)) return CorruptState();
    bool found = false;
    for (std::size_t j = 0; j < clients.size(); ++j)
      for (std::size_t k = 0; k < clients[j].order_count; ++k)
        if (clients[j].orders[k].core_id == order.client_order_id) {
          if (found) return CorruptState();
          found = true;
          BindOwner(i, {order.client_order_id, j, k});
        }
    if (!found) return CorruptState();
  }
  return {};
}

base::Status Server::Storage::RecoverMock() noexcept {
  // Both histories come from the same valid local prefix. They are simulation
  // evidence only; a real adapter must use independent complete broker history.
  trading.AbandonRecoveredRequests();
  while (trading.has_local_updates()) {
    strategyd::TradingDispatch dispatch;
    const auto code = trading.ApplyLocalUpdate(dispatch);
    if (code != base::ErrorCode::kOk) return {code, {}};
  }
  for (std::size_t i = 0; i < mock.recovery_event_count(); ++i) {
    strategyd::TradingEvent event;
    auto status = mock.ReadRecoveryEvent(i, event);
    if (!status.ok()) return status;
    model::OrderSnapshot order;
    if (!trading.TryGetOrder(event.client_order_id, order))
      return CorruptState();
    if (event.report_sequence <= order.report_sequence) continue;
    event.receive_time_ns = clock_.Now();
    if (event.kind == model::OrderUpdateKind::kFill)
      event.fill.receive_time_ns = event.receive_time_ns;
    strategyd::TradingDispatch dispatch;
    const auto code = trading.ApplyEvent(event, dispatch);
    if (code != base::ErrorCode::kOk) return {code, {}};
  }
  const auto now = clock_.Now();
  auto status = mock.RebaseDeadlines(last_record_time, now);
  if (!status.ok()) return status;
  for (std::size_t i = 0; i < trading.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading.TryGetOrderAt(i, order)) return CorruptState();
    bool found = false;
    for (std::size_t j = 0; j < mock.recovery_order_count(); ++j) {
      model::OrderSnapshot broker;
      status = mock.ReadRecoveryOrder(j, broker);
      if (!status.ok()) return status;
      found |= broker.client_order_id == order.client_order_id;
    }
    if (!found && !model::IsTerminalOrderState(order.state)) {
      // A journaled send intent without a mock Send record has no external
      // simulation side effect. Do not send it for the first time on restart.
      strategyd::TradingEvent rejected;
      rejected.client_order_id = order.client_order_id;
      rejected.report_sequence = order.report_sequence + 1;
      rejected.kind = model::OrderUpdateKind::kRejected;
      rejected.receive_time_ns = now;
      rejected.reason.Set(0, "mock request absent from recovered prefix");
      strategyd::TradingDispatch dispatch;
      const auto code = trading.ApplyEvent(rejected, dispatch);
      if (code != base::ErrorCode::kOk) return {code, {}};
    }
  }
  trading.SetRunning(true);
  std::uint64_t recovery_now = now;
  for (std::size_t step = 0; step < kRecoveryStepBudget; ++step) {
    for (std::size_t i = 0; i < trading.order_count(); ++i) {
      model::OrderSnapshot order;
      if (!trading.TryGetOrderAt(i, order)) return CorruptState();
      if (order.cancel_pending) {
        bool matched = false;
        for (std::size_t j = 0; j < mock.recovery_order_count(); ++j) {
          model::OrderSnapshot broker;
          status = mock.ReadRecoveryOrder(j, broker);
          if (!status.ok()) return status;
          if (broker.client_order_id != order.client_order_id) continue;
          matched = true;
          if (!broker.cancel_pending) {
            // The simulated broker proves that the recorded cancel attempt
            // did not reach it. Settle that exact pending intent locally.
            status =
                mock.SendCancel(order.client_order_id, broker.broker_order_id,
                                order.cancel_request_id, recovery_now);
            if (!status.ok()) return status;
          }
          break;
        }
        if (!matched) return CorruptState();
      } else if (order.state == model::OrderState::kWorking) {
        trading.BeginCallback(strategyd::CallbackPhase::kTick);
        const auto code = trading.CancelOrder(order.client_order_id);
        trading.EndCallback();
        if (code != base::ErrorCode::kOk) return {code, {}};
      }
    }
    status = trading.ProcessRequests(adapter, recovery_now);
    if (!status.ok()) return status;
    if (trading.has_local_updates()) {
      strategyd::TradingDispatch dispatch;
      const auto code = trading.ApplyLocalUpdate(dispatch);
      if (code != base::ErrorCode::kOk) return {code, {}};
      continue;
    }
    if (trading.is_drained() && mock.is_drained()) return {};
    const auto deadline = mock.next_deadline();
    if (!deadline) return CorruptState();
    // Advance only the simulation schedule, keeping emitted receive times
    // in the current process clock domain.
    recovery_now = clock_.Now();
    status = mock.RebaseDeadlines(*deadline, recovery_now);
    if (!status.ok()) return status;
    strategyd::TradingEvent event;
    const auto polled = mock.Poll(recovery_now, event);
    if (polled.state != strategyd::TradingPollState::kEvent)
      return CorruptState();
    strategyd::TradingDispatch dispatch;
    const auto code = trading.ApplyEvent(event, dispatch);
    if (code != base::ErrorCode::kOk) return {code, {}};
  }
  return {base::ErrorCode::kResourceExhausted, {}};
}

base::Status Server::Storage::StartPersistence() noexcept {
  if (!config.persistence.enabled()) return {};
  checkpoint.reset(new (std::nothrow) std::byte[kJournalMaxSnapshotBytes]);
  if (!checkpoint) return {base::ErrorCode::kResourceExhausted, {}};
  // Fault in the reusable image on the cold path, before periodic capture.
  std::memset(checkpoint.get(), 0, kJournalMaxSnapshotBytes);
  const auto& p = config.persistence;
  JournalOptions options{p.directory, p.wal_segment_bytes, p.reserve_segments,
                         p.sync_interval_ms, p.sync_after_bytes};
  auto status = journal.Acquire(options);
  auto failure = [&](base::Status result, const char* detail) noexcept {
    std::snprintf(persistence_detail, sizeof(persistence_detail), "%s", detail);
    return result;
  };
  if (!status.ok()) return failure(status, "cannot lock state directory");
  if (p.initialize) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(p.directory, error), end;
         !error && it != end; it.increment(error))
      if (it->path().filename() != "LOCK")
        return failure({base::ErrorCode::kInvalidState, {}},
                       "state directory is not empty; initialization refused");
    if (error) return {base::ErrorCode::kIoError, error};
    if (getentropy(ledger_id.data(), sizeof(ledger_id)) != 0 ||
        (ledger_id[0] | ledger_id[1]) == 0)
      return {base::ErrorCode::kIoError, {}};
  } else {
    status = LoadIdentity();
    if (!status.ok())
      return failure(status,
                     "invalid/missing IDENTITY or account profile mismatch");
    std::uint64_t stream = 0, covered = 0;
    status = LoadCheckpoint(stream, covered);
    if (!status.ok())
      return failure(status, "invalid/missing CURRENT or checkpoint");
    JournalReplayResult result;
    replaying = true;
    status = journal.ReplayFrom(replay_cursor, ReplayRecord, this, result);
    replaying = false;
    if (!status.ok())
      return failure(status, "invalid journal transition during replay");
    // A snapshot may cover records beyond a physically surviving WAL prefix.
  }
  if (last_namespace == UINT64_MAX ||
      order_id_highwater > UINT64_MAX - strategyd::kMaxTradingOrders ||
      cancel_id_highwater > UINT64_MAX - strategyd::kMaxMockTradingReports)
    return {base::ErrorCode::kResourceExhausted, {}};
  run_id = ++last_namespace;
  const auto next_order = order_id_highwater + 1;
  order_id_highwater += strategyd::kMaxTradingOrders;
  const auto next_cancel = cancel_id_highwater + 1;
  cancel_id_highwater += strategyd::kMaxMockTradingReports;
  status = SaveIdentity();
  if (!status.ok()) return status;
  trading.SetRunning(false);
  status = trading.SetNextOrderIdAtLeast(next_order);
  if (status.ok()) status = trading.SetNextCancelIdAtLeast(next_cancel);
  if (!status.ok()) return status;
  if (!p.initialize) {
    status = RecoverMock();
    if (!status.ok())
      return failure(status, "cannot reconcile recovered mock orders");
  }
  status = journal.Start(run_id);
  if (!status.ok()) return status;
  account.revision = 1;
  (void)trading.TryGetCash(account.cash);
  account.position_count = 0;
  for (std::size_t i = 0; i < config.market_data.instrument_count; ++i) {
    model::PositionSnapshot position;
    if (trading.TryGetPosition(config.market_data.instruments[i].instrument.id,
                               position) &&
        (position.total_qty != 0 || position.reserved_qty != 0))
      account.positions[account.position_count++] = position;
  }
  status = CaptureCheckpoint(clock_.Now());
  if (status.ok()) status = PublishCheckpoint();
  if (!status.ok()) return status;
  trading.SetMutationSink(this);
  mock.SetMutationObserver(ObserveMock, this);
  next_snapshot_at = clock_.Now() + config.persistence.snapshot_interval_ms *
                                        kNanosecondsPerMillisecond;
  if (pthread_create(&checkpoint_thread, nullptr, CheckpointEntry, this) != 0)
    return {base::ErrorCode::kResourceExhausted, {}};
  checkpoint_thread_started = true;
  return {};
}

void* Server::Storage::CheckpointEntry(void* context) noexcept {
  auto& s = *static_cast<Storage*>(context);
  while (!s.checkpoint_stop.load(std::memory_order_acquire)) {
    if (s.checkpoint_pending.load(std::memory_order_acquire)) {
      if (!s.PublishCheckpoint().ok())
        s.checkpoint_failed.store(true, std::memory_order_release);
      s.checkpoint_pending.store(false, std::memory_order_release);
    } else {
      usleep(1000);
    }
  }
  return nullptr;
}
void Server::Storage::WaitCheckpoint() noexcept {
  while (checkpoint_pending.load(std::memory_order_acquire))
    std::this_thread::yield();
}
void Server::Storage::PollCheckpoint(std::uint64_t now) noexcept {
  if (!config.persistence.enabled()) return;
  if (checkpoint_failed.load(std::memory_order_acquire) || !journal.healthy()) {
    if (!degraded) {
      degraded = true;
      persistence_failure = {base::ErrorCode::kIoError, {}};
      std::snprintf(persistence_detail, sizeof(persistence_detail),
                    "background storage failure");
    }
  }
  if (degraded || now < next_snapshot_at ||
      checkpoint_pending.load(std::memory_order_acquire))
    return;
  auto status = CaptureCheckpoint(now);
  if (!status.ok()) {
    degraded = true;
    persistence_failure = status;
    std::snprintf(persistence_detail, sizeof(persistence_detail),
                  "checkpoint capture failed");
    return;
  }
  next_snapshot_at = now + config.persistence.snapshot_interval_ms *
                               kNanosecondsPerMillisecond;
  checkpoint_pending.store(true, std::memory_order_release);
}
void Server::Storage::StopCheckpoint() noexcept {
  if (!checkpoint_thread_started) return;
  WaitCheckpoint();
  checkpoint_stop.store(true, std::memory_order_release);
  pthread_join(checkpoint_thread, nullptr);
  checkpoint_thread_started = false;
}

}  // namespace yquant::tradingd
