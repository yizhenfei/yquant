#include <stddef.h>
#include <stdint.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <thread>

#include "tradingd/recovery.h"
#include "tradingd/server.h"
#include "tradingd/state_codec.h"
#include "yquant/base/file.h"
#include "yquant/base/format.h"

namespace yquant::tradingd {
namespace {
inline constexpr uint64_t kGatewayStateVersion = 1;
// Version 2 freezes the complete catalog definition, including trading units.
// The former ID/precision-only identity profile is rejected during load.
inline constexpr uint64_t kIdentityVersion = 2;
inline constexpr uint16_t kLedgerRecord = 1;
inline constexpr uint16_t kMockRecord = 2;
inline constexpr uint16_t kRejectedRecord = 3;
inline constexpr size_t kIdentityBufferBytes = kArchiveIdentityBytes;
inline constexpr size_t kControlBufferBytes = 256;
inline constexpr size_t kSnapshotNameBytes = 96;
inline constexpr size_t kRecoveryStepBudget =
    32 * trading::kMaxMockTradingReports;

base::Status CorruptState() noexcept { return {base::ErrorCode::kCorrupt, {}}; }

// Writes catalog fields explicitly so padding and host ABI are never persisted.
void WriteInstrumentIdentity(
    StateWriter& writer,
    const model::InstrumentDefinition& definition) noexcept {
  writer.U64(definition.id.value);
  writer.U64(static_cast<unsigned>(definition.venue));
  writer.Text(definition.symbol_view());
  writer.U64(definition.price_decimals);
  writer.U64(static_cast<unsigned>(definition.type));
  writer.U64(static_cast<uint64_t>(definition.price_tick));
  writer.U64(static_cast<uint64_t>(definition.quantity_step));
  writer.U64(static_cast<uint64_t>(definition.contract_multiplier));
  writer.U64(static_cast<uint64_t>(definition.margin_rate_ppm));
  writer.U64(static_cast<uint64_t>(definition.open_fee_per_lot));
  writer.U64(static_cast<uint64_t>(definition.close_today_fee_per_lot));
  writer.U64(static_cast<uint64_t>(definition.close_yesterday_fee_per_lot));
}

// A persisted definition must match the validated active catalog exactly.
// Initialization fails on any mismatch or malformed field.
bool MatchesInstrumentIdentity(
    StateReader& reader,
    const model::InstrumentDefinition& definition) noexcept {
  if (reader.U64() != definition.id.value ||
      reader.U64() != static_cast<unsigned>(definition.venue)) {
    return false;
  }
  char symbol[model::kSymbolCapacity] = {};
  return reader.Text(symbol, sizeof(symbol)) &&
         std::string_view(symbol) == definition.symbol_view() &&
         reader.U64() == definition.price_decimals &&
         reader.U64() == static_cast<unsigned>(definition.type) &&
         reader.U64() == static_cast<uint64_t>(definition.price_tick) &&
         reader.U64() == static_cast<uint64_t>(definition.quantity_step) &&
         reader.U64() ==
             static_cast<uint64_t>(definition.contract_multiplier) &&
         reader.U64() == static_cast<uint64_t>(definition.margin_rate_ppm) &&
         reader.U64() == static_cast<uint64_t>(definition.open_fee_per_lot) &&
         reader.U64() ==
             static_cast<uint64_t>(definition.close_today_fee_per_lot) &&
         reader.U64() ==
             static_cast<uint64_t>(definition.close_yesterday_fee_per_lot) &&
         reader.ok();
}

// The wire codec already validates all order/account fields and avoids native
// layouts. Token-bearing Hello messages are never used in persistent records.
base::Status WriteMessage(StateWriter& writer,
                          const trading::Message& message) noexcept {
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t size = 0;
  // Persistent copies do not traverse trading::Connection::Queue, which
  // normally assigns the transport sequence. Use a valid envelope for the
  // shared codec; this sequence is never replayed to a client.
  auto encoded = message;
  if (encoded.sequence == 0) {
    encoded.sequence = 1;
  }
  auto status = trading::EncodeMessage(encoded, bytes, size);
  if (!status.ok()) {
    return status;
  }
  writer.U64(size);
  writer.Bytes(std::span(bytes).first(size));
  return writer.ok() ? base::Status{}
                     : base::Status{base::ErrorCode::kResourceExhausted, {}};
}
base::Status ReadMessage(StateReader& reader,
                         trading::Message& message) noexcept {
  const auto size = reader.U64();
  if (!reader.ok() || size > trading::kMaxFrameSize) {
    return CorruptState();
  }
  const auto bytes = reader.Bytes(size);
  if (!reader.ok()) {
    return CorruptState();
  }
  return DecodeStoredStrategyReport(bytes, message);
}
}  // namespace

namespace {
// Fixed half-full table keeps external stable IDs independent of slot indices.
size_t OwnerBucket(uint64_t id, size_t capacity) noexcept {
  id = (id ^ (id >> 30)) * 0xbf58476d1ce4e5b9ULL;
  id = (id ^ (id >> 27)) * 0x94d049bb133111ebULL;
  return (id ^ (id >> 31)) & (capacity - 1);
}
}  // namespace
size_t Server::OwnerIndex(model::ClientOrderId id) const noexcept {
  auto bucket = OwnerBucket(id.value, owner_lookup_.size());
  for (size_t i = 0; i < owner_lookup_.size(); ++i) {
    const auto value = owner_lookup_[bucket];
    if (value == 0) {
      return owners_.size();
    }
    if (owners_[value - 1].core_id == id) {
      return value - 1;
    }
    bucket = (bucket + 1) & (kOwnerLookupCapacity - 1);
  }
  return owners_.size();
}
void Server::BindOwner(size_t slot, const OrderOwner& owner) noexcept {
  owners_[slot] = owner;
  auto bucket = OwnerBucket(owner.core_id.value, owner_lookup_.size());
  while (owner_lookup_[bucket] != 0 && owner_lookup_[bucket] != slot + 1) {
    bucket = (bucket + 1) & (kOwnerLookupCapacity - 1);
  }
  owner_lookup_[bucket] = static_cast<uint16_t>(slot + 1);
}

base::Status Server::Adapter::SendOrder(model::ClientOrderId id,
                                        const model::OrderRequest& request,
                                        uint64_t now) noexcept {
  if (server_->config_.trading.adapter == trading::TradingAdapterKind::kCtp) {
    return server_->ctp_.SendOrder(id, request, now);
  }
  if (server_->config_.trading.adapter ==
      trading::TradingAdapterKind::kSimulatedFutures) {
    return server_->futures_.SendOrder(id, request, now);
  }
  if (!server_->config_.persistence.enabled()) {
    return server_->mock_.SendOrder(id, request, now);
  }
  const auto index = server_->OwnerIndex(id);
  if (index == server_->owners_.size()) {
    return CorruptState();
  }
  const auto owner = server_->owners_[index];
  char key[kAdapterRequestKeyBytes + 1];
  const auto status = EncodeAdapterRequestKey(
      server_->clients_[owner.client].session_id, AdapterRequestKind::kNewOrder,
      owner.local + 1, key);
  if (!status.ok()) {
    return status;
  }
  return server_->mock_.SendOrderWithIdentity(id, request, key, now);
}

base::Status Server::Append(uint16_t type, std::span<const std::byte> payload,
                            bool new_risk) noexcept {
  if (replaying_ || !config_.persistence.enabled()) {
    return {};
  }
  base::Status status = persistence_failure_;
  if (!degraded_) {
    uint64_t lsn = 0;
    status = journal_.TryAppend(type, payload, lsn);
    if (status.ok()) {
      return {};
    }
    degraded_ = true;
    persistence_failure_ = status;
    ::yquant::base::FormatTo(persistence_detail_, sizeof(persistence_detail_),
                             "journal append failed: {}",
                             journal_.error_detail());
  }
  // True broker facts and risk-reducing cancels still commit in memory. New
  // orders/sends fail before their ledger change or external side effect.
  return new_risk ? status : base::Status{};
}

base::Status Server::BeforeCommit(
    const trading::TradingMutation& mutation) noexcept {
  if (!config_.persistence.enabled() || replaying_) {
    return {};
  }
  using Kind = trading::TradingMutationKind;
  size_t client_index = context_client_;
  size_t local_index = context_local_;
  auto cancel = context_cancel_;
  if (mutation.kind != Kind::kNewOrder &&
      context_client_ == trading::kMaxClients) {
    const auto index = OwnerIndex(mutation.client_order_id);
    if (index == owners_.size()) {
      return CorruptState();
    }
    client_index = owners_[index].client;
    local_index = owners_[index].local;
    cancel = clients_[client_index].orders[local_index].local_cancel;
  }
  if (client_index >= config_.max_clients ||
      local_index >= trading::kMaxTradingOrders) {
    return CorruptState();
  }
  std::array<std::byte, kJournalMaxPayloadBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(client_index);
  writer.U64(local_index);
  writer.U64(cancel.value);
  std::array<std::byte, trading::kMaxEncodedTradingMutation> fact{};
  size_t size = 0;
  auto status = trading::EncodeTradingMutation(mutation, fact, size);
  if (!status.ok()) {
    return status;
  }
  writer.Bytes(std::span(fact).first(size));
  status = Append(
      kLedgerRecord, std::span(bytes).first(writer.size()),
      mutation.kind == Kind::kNewOrder || mutation.kind == Kind::kSendNew);
  if (status.ok()) {
    last_record_time_ = mutation.now;
  }
  return status;
}

base::Status Server::ObserveMock(
    void* context, const trading::MockTradingMutation& mutation) noexcept {
  auto& server = *static_cast<Server*>(context);
  std::array<std::byte, trading::kMaxMockTradingMutationBytes> bytes{};
  size_t size = 0;
  auto status = trading::EncodeMockTradingMutation(mutation, bytes, size);
  if (!status.ok()) {
    return status;
  }
  status = server.Append(
      kMockRecord, std::span(bytes).first(size),
      mutation.kind == trading::MockTradingMutationKind::kSendOrder);
  if (status.ok()) {
    server.last_record_time_ = mutation.now;
  }
  return status;
}

base::Status Server::PersistRejection(
    size_t client, size_t local, const trading::Message& message) noexcept {
  if (!config_.persistence.enabled()) {
    return {};
  }
  std::array<std::byte, kJournalMaxPayloadBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(client);
  writer.U64(local);
  auto status = WriteMessage(writer, message);
  if (!status.ok()) {
    return status;
  }
  return Append(kRejectedRecord, std::span(bytes).first(writer.size()), false);
}

base::Status Server::ReplayRecord(void* context, uint16_t type, uint64_t,
                                  std::span<const std::byte> payload) noexcept {
  auto& server = *static_cast<Server*>(context);
  if (type == kMockRecord) {
    trading::MockTradingMutation mutation;
    auto status = trading::DecodeMockTradingMutation(payload, mutation);
    if (status.ok()) {
      status = server.mock_.ReplayMutation(mutation);
    }
    if (status.ok()) {
      server.last_record_time_ = mutation.now;
    }
    return status;
  }
  StateReader reader(payload);
  const auto client_index = reader.U64();
  const auto local_index = reader.U64();
  if (!reader.ok() || client_index >= server.config_.max_clients ||
      local_index >= trading::kMaxTradingOrders ||
      server.clients_[client_index].id == 0) {
    return CorruptState();
  }
  auto& client = server.clients_[client_index];
  auto& local = client.orders[local_index];
  client.order_count =
      std::max(client.order_count, static_cast<size_t>(local_index + 1));
  if (type == kRejectedRecord) {
    trading::Message message;
    auto status = ReadMessage(reader, message);
    if (!status.ok() || !reader.done() ||
        message.type != trading::MessageType::kOrderUpdate ||
        message.update.order.strategy_order_id.value != local_index + 1) {
      return CorruptState();
    }
    if (local.core_id.value == 0) {
      local.rejected = message.update.order;
    }
    local.report_sequence = message.update.order.report_sequence;
    client.last_cancel_id =
        std::max(client.last_cancel_id, message.update.cancel_request_id.value);
    local.local_cancel = message.update.cancel_request_id;
    return {};
  }
  if (type != kLedgerRecord) {
    return CorruptState();
  }
  const auto local_cancel = reader.U64();
  trading::TradingMutation mutation;
  const auto encoded = reader.Bytes(payload.size() - 24);
  if (!reader.done()) {
    return CorruptState();
  }
  auto status = trading::DecodeTradingMutation(encoded, mutation);
  if (!status.ok()) {
    return status;
  }
  trading::TradingDispatch dispatch;
  status = server.trading_.ReplayMutation(mutation, &dispatch);
  if (!status.ok()) {
    return status;
  }
  using Kind = trading::TradingMutationKind;
  if (mutation.kind == Kind::kNewOrder) {
    const auto index = server.trading_.order_count() - 1;
    server.BindOwner(
        index, {mutation.client_order_id, static_cast<size_t>(client_index),
                static_cast<size_t>(local_index)});
    local.core_id = mutation.client_order_id;
  } else if (mutation.kind == Kind::kCancel) {
    local.core_cancel = mutation.cancel_id;
    local.local_cancel = {local_cancel};
    client.last_cancel_id = std::max(client.last_cancel_id, local_cancel);
  }
  if (dispatch.update.order.client_order_id.value != 0) {
    ++local.report_sequence;
  }
  server.last_record_time_ = mutation.now;
  return {};
}

base::Status Server::SaveIdentity() noexcept {
  WaitCheckpoint();
  std::array<std::byte, kIdentityBufferBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(kIdentityVersion);
  writer.Text(config_.persistence.account_key);
  for (const auto value : ledger_id_) {
    writer.U64(value);
  }
  writer.U64(last_namespace_);
  writer.U64(order_id_highwater_);
  writer.U64(cancel_id_highwater_);
  writer.U64(config_.market_data.instrument_count);
  for (size_t i = 0; i < config_.market_data.instrument_count; ++i) {
    const auto& instrument = config_.market_data.instruments[i].instrument;
    WriteInstrumentIdentity(writer, instrument);
  }
  writer.U64(static_cast<unsigned>(config_.trading.mode));
  writer.U64(config_.trading.report_interval_ns);
  for (const auto& client : clients_) {
    writer.U64(client.id);
    writer.U64(client.session_id);
    writer.U64(client.instrument_count);
    for (size_t j = 0; j < client.instrument_count; ++j) {
      writer.U64(client.instruments[j].id.value);
      writer.U64(client.instruments[j].price_decimals);
    }
  }
  if (!writer.ok()) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  auto status =
      journal_.SaveFile("IDENTITY", std::span(bytes).first(writer.size()));
  if (status.ok() && config_.mysql_archive.enabled) {
    char name[96];
    ArchiveName(name, "identity", last_namespace_);
    status = journal_.SaveFile(name, std::span(bytes).first(writer.size()));
  }
  return status;
}

base::Status Server::LoadIdentity() noexcept {
  std::array<std::byte, kIdentityBufferBytes> bytes{};
  size_t size = 0;
  auto status = journal_.LoadFile("IDENTITY", bytes, size);
  if (!status.ok()) {
    return status;
  }
  StateReader reader{std::span(bytes).first(size)};
  if (reader.U64() != kIdentityVersion) {
    return CorruptState();
  }
  char key[kMaxAccountKeyBytes + 1];
  if (!reader.Text(key, sizeof(key)) ||
      std::strcmp(key, config_.persistence.account_key) != 0) {
    return CorruptState();
  }
  for (auto& value : ledger_id_) {
    value = reader.U64();
  }
  last_namespace_ = reader.U64();
  order_id_highwater_ = reader.U64();
  cancel_id_highwater_ = reader.U64();
  if ((ledger_id_[0] | ledger_id_[1]) == 0 ||
      reader.U64() != config_.market_data.instrument_count) {
    return CorruptState();
  }
  for (size_t i = 0; i < config_.market_data.instrument_count; ++i) {
    const auto& instrument = config_.market_data.instruments[i].instrument;
    if (!MatchesInstrumentIdentity(reader, instrument)) {
      return CorruptState();
    }
  }
  if (reader.U64() != static_cast<unsigned>(config_.trading.mode) ||
      reader.U64() != config_.trading.report_interval_ns) {
    return CorruptState();
  }
  for (size_t i = 0; i < clients_.size(); ++i) {
    auto& client = clients_[i];
    client.id = reader.U64();
    client.session_id = reader.U64();
    client.instrument_count = reader.U64();
    if (client.instrument_count > trading::kMaxInstruments ||
        ((client.id == 0) != (client.session_id == 0)) ||
        client.session_id > last_namespace_ ||
        (client.id != 0 && i >= config_.max_clients)) {
      return CorruptState();
    }
    for (size_t j = 0; j < client.instrument_count; ++j) {
      const auto id = reader.U64();
      const auto decimals = reader.U64();
      if (decimals > UINT8_MAX) {
        return CorruptState();
      }
      client.instruments[j] = {{id}, static_cast<uint8_t>(decimals)};
      model::InstrumentDefinition known;
      if (!instruments_.TryGetInstrument(client.instruments[j].id, known) ||
          known.price_decimals != decimals) {
        return CorruptState();
      }
    }
    if (client.id != 0) {
      if (client.instrument_count == 0) {
        return CorruptState();
      }
      client.authenticated = true;
      client.stopping = true;
      for (size_t j = 0; j < i; ++j) {
        if (clients_[j].id == client.id ||
            clients_[j].session_id == client.session_id) {
          return CorruptState();
        }
      }
    }
  }
  return reader.done() ? base::Status{} : CorruptState();
}

base::Status Server::RegisterClient(size_t index) noexcept {
  if (!config_.persistence.enabled()) {
    return {};
  }
  if (last_namespace_ == UINT64_MAX) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  clients_[index].session_id = ++last_namespace_;
  auto status = SaveIdentity();
  if (!status.ok()) {
    degraded_ = true;
    persistence_failure_ = status;
    ::yquant::base::FormatTo(persistence_detail_, sizeof(persistence_detail_),
                             "client identity sync failed");
  }
  return status;
}

base::Status Server::CaptureCheckpoint(uint64_t now) noexcept {
  if (degraded_) {
    return persistence_failure_;
  }
  StateWriter writer{std::span(checkpoint_.get(), kJournalMaxSnapshotBytes)};
  writer.U64(kGatewayStateVersion);
  for (const auto id : ledger_id_) {
    writer.U64(id);
  }
  writer.U64(run_id_);
  checkpoint_cursor_ = journal_.GetCursor();
  checkpoint_lsn_ = checkpoint_cursor_.lsn;
  writer.U64(checkpoint_lsn_);
  writer.U64(checkpoint_cursor_.segment);
  writer.U64(checkpoint_cursor_.offset);
  writer.U64(now);
  for (const auto& client : clients_) {
    writer.U64(client.id);
    writer.U64(client.session_id);
    writer.U64(client.order_count);
    writer.U64(client.last_cancel_id);
    for (size_t i = 0; i < client.order_count; ++i) {
      const auto& local = client.orders[i];
      writer.U64(local.core_id.value);
      writer.U64(local.report_sequence);
      writer.U64(local.core_cancel.value);
      writer.U64(local.local_cancel.value);
      writer.U64(local.retry_at);
      if (local.core_id.value == 0) {
        trading::Message message;
        message.type = trading::MessageType::kOrderUpdate;
        message.update.kind = model::OrderUpdateKind::kRejected;
        message.update.order = local.rejected;
        message.account = account_;
        auto status = WriteMessage(writer, message);
        if (!status.ok()) {
          return status;
        }
      }
    }
  }
  // Each codec writes directly into the immutable checkpoint buffer. Only its
  // length prefix is patched; there is no temporary full-state copy.
  auto encode_blob = [&](auto encode) noexcept -> base::Status {
    if (writer.tail().size() < sizeof(uint64_t)) {
      return base::Status{base::ErrorCode::kResourceExhausted, {}};
    }
    auto prefix = writer.tail().first(sizeof(uint64_t));
    writer.U64(0);
    size_t size = 0;
    auto status = encode(writer.tail(), size);
    if (!status.ok()) {
      return status;
    }
    StateWriter length(prefix);
    length.U64(size);
    writer.Advance(size);
    return {};
  };
  if (!writer.ok()) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  auto status = encode_blob([&](auto out, auto& size) noexcept {
    return trading_.ExportCheckpoint(out, size);
  });
  if (status.ok()) {
    status = encode_blob([&](auto out, auto& size) noexcept {
      return mock_.EncodeState(now, out, size);
    });
  }
  if (!status.ok()) {
    return status;
  }
  if (!writer.ok()) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  checkpoint_size_ = writer.size();
  return {};
}

base::Status Server::PublishCheckpoint() noexcept {
  char name[kSnapshotNameBytes];
  ::yquant::base::FormatTo(name, sizeof(name), "snapshot-{}-{}.bin", run_id_,
                           checkpoint_lsn_);
  auto status = journal_.SaveFile(name, {checkpoint_.get(), checkpoint_size_});
  if (!status.ok()) {
    return status;
  }
  std::array<std::byte, kControlBufferBytes> bytes{};
  StateWriter writer(bytes);
  writer.U64(kGatewayStateVersion);
  writer.U64(run_id_);
  writer.U64(checkpoint_lsn_);
  writer.U64(checkpoint_cursor_.segment);
  writer.U64(checkpoint_cursor_.offset);
  writer.Text(name);
  return journal_.SaveFile("CURRENT", std::span(bytes).first(writer.size()));
}

base::Status Server::LoadCheckpoint(uint64_t& stream,
                                    uint64_t& covered) noexcept {
  std::array<std::byte, kControlBufferBytes> bytes{};
  size_t size = 0;
  auto status = journal_.LoadFile("CURRENT", bytes, size);
  if (!status.ok()) {
    return status;
  }
  StateReader control{std::span(bytes).first(size)};
  if (control.U64() != kGatewayStateVersion) {
    return CorruptState();
  }
  stream = control.U64();
  covered = control.U64();
  replay_cursor_ = {stream, control.U64(), control.U64(), covered};
  char name[kSnapshotNameBytes];
  if (!control.Text(name, sizeof(name)) || !control.done() || stream == 0 ||
      stream > last_namespace_) {
    return CorruptState();
  }
  status = journal_.LoadFile(
      name, {checkpoint_.get(), kJournalMaxSnapshotBytes}, size);
  if (!status.ok()) {
    return status;
  }
  StateReader reader{std::span(checkpoint_.get(), size)};
  if (reader.U64() != kGatewayStateVersion) {
    return CorruptState();
  }
  for (const auto id : ledger_id_) {
    if (reader.U64() != id) {
      return CorruptState();
    }
  }
  if (reader.U64() != stream || reader.U64() != covered ||
      reader.U64() != replay_cursor_.segment ||
      reader.U64() != replay_cursor_.offset) {
    return CorruptState();
  }
  last_record_time_ = reader.U64();
  for (auto& client : clients_) {
    const auto id = reader.U64();
    const auto session = reader.U64();
    if (id != 0 && (id != client.id || session != client.session_id)) {
      return CorruptState();
    }
    const auto count = reader.U64();
    client.last_cancel_id = reader.U64();
    if (count > client.orders.size() || (id == 0 && count != 0)) {
      return CorruptState();
    }
    client.order_count = count;
    for (size_t i = 0; i < count; ++i) {
      auto& local = client.orders[i];
      local.core_id = {reader.U64()};
      local.report_sequence = reader.U64();
      local.core_cancel = {reader.U64()};
      local.local_cancel = {reader.U64()};
      local.retry_at = reader.U64();
      if (local.core_id.value == 0) {
        trading::Message message;
        status = ReadMessage(reader, message);
        if (!status.ok() ||
            message.type != trading::MessageType::kOrderUpdate ||
            message.update.order.strategy_order_id.value != i + 1) {
          return CorruptState();
        }
        local.rejected = message.update.order;
      }
    }
  }
  const auto ledger_size = reader.U64();
  const auto ledger = reader.Bytes(ledger_size);
  if (!reader.ok()) {
    return CorruptState();
  }
  status = trading_.RestoreCheckpoint(ledger);
  if (!status.ok()) {
    return status;
  }
  const auto mock_size = reader.U64();
  const auto mock_state = reader.Bytes(mock_size);
  if (!reader.done()) {
    return CorruptState();
  }
  status = mock_.RestoreState(mock_state, last_record_time_);
  if (!status.ok()) {
    return status;
  }
  for (size_t i = 0; i < trading_.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading_.TryGetOrderAt(i, order)) {
      return CorruptState();
    }
    bool found = false;
    for (size_t j = 0; j < clients_.size(); ++j) {
      for (size_t k = 0; k < clients_[j].order_count; ++k) {
        if (clients_[j].orders[k].core_id == order.client_order_id) {
          if (found) {
            return CorruptState();
          }
          found = true;
          BindOwner(i, {order.client_order_id, j, k});
        }
      }
    }
    if (!found) {
      return CorruptState();
    }
  }
  return {};
}

base::Status Server::ReplayMockHistory() noexcept {
  // Both histories come from the same valid local prefix. They are simulation
  // evidence only; a real adapter must use independent complete broker history.
  trading_.AbandonRecoveredRequests();
  while (trading_.has_local_updates()) {
    trading::TradingDispatch dispatch;
    const auto code = trading_.ApplyLocalUpdate(dispatch);
    if (code != base::ErrorCode::kOk) {
      return {code, {}};
    }
  }
  for (size_t i = 0; i < mock_.recovery_event_count(); ++i) {
    model::TradingEvent event;
    auto status = mock_.ReadRecoveryEvent(i, event);
    if (!status.ok()) {
      return status;
    }
    model::OrderSnapshot order;
    if (!trading_.TryGetOrder(event.client_order_id, order)) {
      return CorruptState();
    }
    if (event.report_sequence <= order.report_sequence) {
      continue;
    }
    event.receive_time_ns = clock_.Now();
    if (event.kind == model::OrderUpdateKind::kFill) {
      event.fill.receive_time_ns = event.receive_time_ns;
    }
    trading::TradingDispatch dispatch;
    const auto code = trading_.ApplyEvent(event, dispatch);
    if (code != base::ErrorCode::kOk) {
      return {code, {}};
    }
  }
  return {};
}

base::Status Server::RejectAbsentMockOrders(uint64_t now) noexcept {
  for (size_t i = 0; i < trading_.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading_.TryGetOrderAt(i, order)) {
      return CorruptState();
    }
    bool found = false;
    for (size_t j = 0; j < mock_.recovery_order_count(); ++j) {
      model::OrderSnapshot broker;
      const auto status = mock_.ReadRecoveryOrder(j, broker);
      if (!status.ok()) {
        return status;
      }
      found |= broker.client_order_id == order.client_order_id;
    }
    if (!found && !model::IsTerminalOrderState(order.state)) {
      // A journaled send intent without a mock Send record has no external
      // simulation side effect. Do not send it for the first time on restart.
      model::TradingEvent rejected;
      rejected.client_order_id = order.client_order_id;
      rejected.report_sequence = order.report_sequence + 1;
      rejected.kind = model::OrderUpdateKind::kRejected;
      rejected.receive_time_ns = now;
      rejected.reason.Set(0, "mock request absent from recovered prefix");
      trading::TradingDispatch dispatch;
      const auto code = trading_.ApplyEvent(rejected, dispatch);
      if (code != base::ErrorCode::kOk) {
        return {code, {}};
      }
    }
  }

  return {};
}

base::Status Server::ScheduleRecoveredCancels(uint64_t now) noexcept {
  for (size_t i = 0; i < trading_.order_count(); ++i) {
    model::OrderSnapshot order;
    if (!trading_.TryGetOrderAt(i, order)) {
      return CorruptState();
    }
    if (order.cancel_pending) {
      bool matched = false;
      for (size_t j = 0; j < mock_.recovery_order_count(); ++j) {
        model::OrderSnapshot broker;
        auto status = mock_.ReadRecoveryOrder(j, broker);
        if (!status.ok()) {
          return status;
        }
        if (broker.client_order_id != order.client_order_id) {
          continue;
        }
        matched = true;
        if (!broker.cancel_pending) {
          // The simulated broker proves that the recorded cancel attempt
          // did not reach it. Settle that exact pending intent locally.
          status =
              mock_.SendCancel(order.client_order_id, broker.adapter_order_id,
                               order.cancel_request_id, now);
          if (!status.ok()) {
            return status;
          }
        }
        break;
      }
      if (!matched) {
        return CorruptState();
      }
    } else if (order.state == model::OrderState::kWorking) {
      trading_.SetAcceptingRequests(true);
      const auto code = trading_.CancelOrder(order.client_order_id);
      trading_.SetAcceptingRequests(false);
      if (code != base::ErrorCode::kOk) {
        return {code, {}};
      }
    }
  }
  return {};
}

base::Status Server::DrainRecoveredMock(uint64_t now) noexcept {
  uint64_t recovery_now = now;
  for (size_t step = 0; step < kRecoveryStepBudget; ++step) {
    auto status = ScheduleRecoveredCancels(recovery_now);
    if (!status.ok()) {
      return status;
    }
    status = trading_.ProcessRequests(recovery_now);
    if (!status.ok()) {
      return status;
    }
    if (trading_.has_local_updates()) {
      trading::TradingDispatch dispatch;
      const auto code = trading_.ApplyLocalUpdate(dispatch);
      if (code != base::ErrorCode::kOk) {
        return {code, {}};
      }
      continue;
    }
    if (trading_.is_drained() && mock_.is_drained()) {
      return {};
    }
    const auto deadline = mock_.next_deadline();
    if (!deadline) {
      return CorruptState();
    }
    // Advance only the simulation schedule, keeping emitted receive times
    // in the current process clock domain.
    recovery_now = clock_.Now();
    status = mock_.RebaseDeadlines(*deadline, recovery_now);
    if (!status.ok()) {
      return status;
    }
    model::TradingEvent event;
    const auto polled = mock_.Poll(recovery_now, event);
    if (polled.state != trading::TradingPollState::kEvent) {
      return CorruptState();
    }
    trading::TradingDispatch dispatch;
    const auto code = trading_.ApplyEvent(event, dispatch);
    if (code != base::ErrorCode::kOk) {
      return {code, {}};
    }
  }
  return {base::ErrorCode::kResourceExhausted, {}};
}

base::Status Server::RecoverMock() noexcept {
  auto status = ReplayMockHistory();
  if (!status.ok()) {
    return status;
  }
  const auto now = clock_.Now();
  status = mock_.RebaseDeadlines(last_record_time_, now);
  if (status.ok()) {
    status = RejectAbsentMockOrders(now);
  }
  if (status.ok()) {
    status = DrainRecoveredMock(now);
  }
  return status;
}

base::Status Server::StartPersistence() noexcept {
  if (!config_.persistence.enabled()) {
    return {};
  }
  checkpoint_.reset(new (std::nothrow) std::byte[kJournalMaxSnapshotBytes]);
  if (!checkpoint_) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  // Fault in the reusable image on the cold path, before periodic capture.
  std::memset(checkpoint_.get(), 0, kJournalMaxSnapshotBytes);
  const auto& p = config_.persistence;
  JournalOptions options{p.directory, p.wal_segment_bytes, p.reserve_segments,
                         p.sync_interval_ms, p.sync_after_bytes};
  auto status = journal_.Acquire(options);
  auto failure = [&](base::Status result, const char* detail) noexcept {
    ::yquant::base::FormatTo(persistence_detail_, sizeof(persistence_detail_),
                             "{}", detail);
    return result;
  };
  if (!status.ok()) {
    return failure(status, "cannot lock state directory");
  }
  if (p.initialize) {
    base::DirectoryReader directory;
    auto directory_status = directory.Open(p.directory);
    if (!directory_status.ok()) {
      return directory_status;
    }
    for (;;) {
      std::string_view name;
      bool done = false;
      directory_status = directory.Next(name, done);
      if (!directory_status.ok()) {
        return directory_status;
      }
      if (done) {
        break;
      }
      if (name != "LOCK") {
        return failure({base::ErrorCode::kInvalidState, {}},
                       "state directory is not empty; initialization refused");
      }
    }
    if (getentropy(ledger_id_.data(), sizeof(ledger_id_)) != 0 ||
        (ledger_id_[0] | ledger_id_[1]) == 0) {
      return {base::ErrorCode::kIoError, {}};
    }
  } else {
    status = LoadIdentity();
    if (!status.ok()) {
      return failure(status,
                     "invalid/missing IDENTITY or account profile mismatch");
    }
    uint64_t stream = 0, covered = 0;
    status = LoadCheckpoint(stream, covered);
    if (!status.ok()) {
      return failure(status, "invalid/missing CURRENT or checkpoint");
    }
    JournalReplayResult result;
    replaying_ = true;
    status = journal_.ReplayFrom(replay_cursor_, ReplayRecord, this, result);
    replaying_ = false;
    if (!status.ok()) {
      return failure(status, "invalid journal transition during replay");
    }
    // A snapshot may cover records beyond a physically surviving WAL prefix.
  }
  if (last_namespace_ == UINT64_MAX ||
      order_id_highwater_ > UINT64_MAX - trading::kMaxTradingOrders ||
      cancel_id_highwater_ > UINT64_MAX - trading::kMaxMockTradingReports) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  run_id_ = ++last_namespace_;
  const auto next_order = order_id_highwater_ + 1;
  order_id_highwater_ += trading::kMaxTradingOrders;
  const auto next_cancel = cancel_id_highwater_ + 1;
  cancel_id_highwater_ += trading::kMaxMockTradingReports;
  status = SaveIdentity();
  if (!status.ok()) {
    return status;
  }
  trading_.SetAcceptingRequests(false);
  status = trading_.SetNextOrderIdAtLeast(next_order);
  if (status.ok()) {
    status = trading_.SetNextCancelIdAtLeast(next_cancel);
  }
  if (!status.ok()) {
    return status;
  }
  if (!p.initialize) {
    status = RecoverMock();
    if (!status.ok()) {
      return failure(status, "cannot reconcile recovered mock orders");
    }
  }
  status = journal_.Start(run_id_);
  if (!status.ok()) {
    return status;
  }
  account_.revision = 1;
  (void)trading_.TryGetCash(account_.cash);
  account_.position_count = 0;
  for (size_t i = 0; i < config_.market_data.instrument_count; ++i) {
    model::PositionSnapshot position;
    if (trading_.TryGetPosition(
            config_.market_data.instruments[i].instrument.id, position) &&
        (position.total_qty != 0 || position.reserved_qty != 0)) {
      account_.positions[account_.position_count++] = position;
    }
  }
  status = CaptureCheckpoint(clock_.Now());
  if (status.ok()) {
    status = PublishCheckpoint();
  }
  if (!status.ok()) {
    return status;
  }
  if (config_.mysql_archive.enabled) {
    status = PublishArchiveBaseline(p.initialize);
    if (status.ok()) {
      status = mysql_archive_.Start(config_, journal_, run_id_);
    }
    if (!status.ok()) {
      return status;
    }
  }
  status = trading_.SetMutationSink(this);
  if (!status.ok()) {
    return status;
  }
  mock_.SetMutationObserver(ObserveMock, this);
  next_snapshot_at_ = clock_.Now() + config_.persistence.snapshot_interval_ms *
                                         kNanosecondsPerMillisecond;
  if (pthread_create(&checkpoint_thread_, nullptr, CheckpointEntry, this) !=
      0) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  checkpoint_thread_started_ = true;
  return {};
}

base::Status Server::SaveArchiveEnd(uint64_t run, uint64_t lsn) noexcept {
  char name[96];
  ArchiveName(name, "end", run);
  std::array<std::byte, 32> bytes{};
  StateWriter writer(bytes);
  writer.U64(kArchiveFormatVersion);
  writer.U64(run);
  writer.U64(lsn);
  // Existing end manifests are immutable. A replay cannot silently shrink or
  // extend a previously published final boundary.
  std::array<std::byte, 32> existing{};
  size_t size = 0;
  auto status = journal_.LoadFile(name, existing, size);
  if (status.ok()) {
    if (size != writer.size() ||
        std::memcmp(existing.data(), bytes.data(), size) != 0) {
      return CorruptState();
    }
    return {};
  }
  if (status.system_error.value() != ENOENT) {
    return status;
  }
  return journal_.SaveFile(name, std::span(bytes).first(writer.size()));
}

base::Status Server::PublishArchiveBaseline(bool initialized) noexcept {
  uint64_t previous = 0;
  for (;;) {
    uint64_t next = 0;
    auto status = NextArchiveRun(config_.persistence.directory, previous, next);
    if (!status.ok()) {
      return status;
    }
    if (next == 0) {
      break;
    }
    if (next >= run_id_) {
      return CorruptState();
    }
    previous = next;
  }
  JournalReplayResult prefix;
  bool gap = !initialized && previous == 0;
  if (previous != 0) {
    auto status = ProbeJournal(config_.persistence.directory, previous, prefix);
    if (!status.ok()) {
      return status;
    }
    status = SaveArchiveEnd(previous, prefix.last_lsn);
    if (!status.ok()) {
      return status;
    }
    gap = prefix.tail_discarded || replay_cursor_.stream_id != previous;
    // A durable checkpoint may cover missing records. Preserve the state while
    // explicitly recording that individual historical events are unavailable.
    gap |= replay_cursor_.stream_id == previous &&
           replay_cursor_.lsn > prefix.last_lsn;
  }
  std::array<std::byte, kIdentityBufferBytes> metadata{};
  size_t metadata_size = 0;
  auto status = journal_.LoadFile("IDENTITY", metadata, metadata_size);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<std::byte[]> bytes(new (std::nothrow)
                                         std::byte[kJournalMaxSnapshotBytes]);
  if (!bytes) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  StateWriter baseline({bytes.get(), kJournalMaxSnapshotBytes});
  baseline.U64(kArchiveFormatVersion);
  for (auto id : ledger_id_) {
    baseline.U64(id);
  }
  baseline.U64(run_id_);
  baseline.Text(config_.persistence.account_key);
  baseline.U64(metadata_size);
  baseline.Bytes(std::span(metadata).first(metadata_size));
  baseline.U64(checkpoint_size_);
  baseline.Bytes({checkpoint_.get(), checkpoint_size_});
  if (!baseline.ok()) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  char name[96];
  ArchiveName(name, "base", run_id_);
  status = journal_.SaveFile(name, {bytes.get(), baseline.size()});
  if (!status.ok()) {
    return status;
  }
  std::array<std::byte, 64> manifest{};
  StateWriter writer(manifest);
  writer.U64(kArchiveFormatVersion);
  writer.U64(run_id_);
  writer.U64(previous);
  writer.U64(prefix.last_lsn);
  writer.U64(gap);
  ArchiveName(name, "run", run_id_);
  return journal_.SaveFile(name, std::span(manifest).first(writer.size()));
}

void* Server::CheckpointEntry(void* context) noexcept {
  auto& server = *static_cast<Server*>(context);
  while (!server.checkpoint_stop_.load(std::memory_order_acquire)) {
    if (server.checkpoint_pending_.load(std::memory_order_acquire)) {
      if (!server.PublishCheckpoint().ok()) {
        server.checkpoint_failed_.store(true, std::memory_order_release);
      }
      server.checkpoint_pending_.store(false, std::memory_order_release);
    } else {
      usleep(1000);
    }
  }
  return nullptr;
}
void Server::WaitCheckpoint() noexcept {
  while (checkpoint_pending_.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
}
void Server::PollCheckpoint(uint64_t now) noexcept {
  if (!config_.persistence.enabled()) {
    return;
  }
  if (checkpoint_failed_.load(std::memory_order_acquire) ||
      !journal_.healthy()) {
    if (!degraded_) {
      degraded_ = true;
      persistence_failure_ = {base::ErrorCode::kIoError, {}};
      ::yquant::base::FormatTo(persistence_detail_, sizeof(persistence_detail_),
                               "background storage failure");
    }
  }
  if (degraded_ || now < next_snapshot_at_ ||
      checkpoint_pending_.load(std::memory_order_acquire)) {
    return;
  }
  auto status = CaptureCheckpoint(now);
  if (!status.ok()) {
    degraded_ = true;
    persistence_failure_ = status;
    ::yquant::base::FormatTo(persistence_detail_, sizeof(persistence_detail_),
                             "checkpoint capture failed");
    return;
  }
  next_snapshot_at_ = now + config_.persistence.snapshot_interval_ms *
                                kNanosecondsPerMillisecond;
  checkpoint_pending_.store(true, std::memory_order_release);
}
void Server::StopCheckpoint() noexcept {
  if (!checkpoint_thread_started_) {
    return;
  }
  WaitCheckpoint();
  checkpoint_stop_.store(true, std::memory_order_release);
  pthread_join(checkpoint_thread_, nullptr);
  checkpoint_thread_started_ = false;
}

void Server::ResetPersistenceState() noexcept {
  run_id_ = 0;
  ledger_id_ = {};
  last_namespace_ = 0;
  order_id_highwater_ = 0;
  cancel_id_highwater_ = 0;
  context_client_ = trading::kMaxClients;
  context_local_ = 0;
  context_cancel_ = {};
  last_record_time_ = 0;
  degraded_ = false;
  archive_disk_pressure_ = false;
  replaying_ = false;
  persistence_failure_ = {};
  persistence_detail_[0] = '\0';
  checkpoint_.reset();
  checkpoint_size_ = 0;
  checkpoint_lsn_ = 0;
  checkpoint_cursor_ = {};
  replay_cursor_ = {};
  next_snapshot_at_ = 0;
  checkpoint_thread_ = {};
  checkpoint_thread_started_ = false;
  checkpoint_pending_.store(false, std::memory_order_relaxed);
  checkpoint_stop_.store(false, std::memory_order_relaxed);
  checkpoint_failed_.store(false, std::memory_order_relaxed);
}

}  // namespace yquant::tradingd
