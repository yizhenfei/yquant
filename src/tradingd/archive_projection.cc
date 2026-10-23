#include "tradingd/archive_projection.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "tradingd/mysql_archive.h"
#include "tradingd/state_codec.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::tradingd {
namespace {
constexpr size_t kMaxArchiveOrders =
    trading::kMaxClients * trading::kMaxTradingOrders;
constexpr uint16_t kLedgerRecord = 1;
constexpr uint16_t kMockRecord = 2;
constexpr uint16_t kRejectedRecord = 3;

base::Status Corrupt() noexcept { return {base::ErrorCode::kCorrupt, {}}; }

// Identity fields use the gateway's explicit codec rather than native layout.
bool MatchInstrument(StateReader& reader,
                     const model::InstrumentDefinition& expected) noexcept {
  if (reader.U64() != expected.id.value ||
      reader.U64() != static_cast<unsigned>(expected.venue)) {
    return false;
  }
  char symbol[model::kSymbolCapacity]{};
  return reader.Text(symbol, sizeof(symbol)) &&
         std::string_view(symbol) == expected.symbol_view() &&
         reader.U64() == expected.price_decimals &&
         reader.U64() == static_cast<unsigned>(expected.type) &&
         reader.U64() == static_cast<uint64_t>(expected.price_tick) &&
         reader.U64() == static_cast<uint64_t>(expected.quantity_step) &&
         reader.U64() == static_cast<uint64_t>(expected.contract_multiplier) &&
         reader.U64() == static_cast<uint64_t>(expected.margin_rate_ppm) &&
         reader.U64() == static_cast<uint64_t>(expected.open_fee_per_lot) &&
         reader.U64() ==
             static_cast<uint64_t>(expected.close_today_fee_per_lot) &&
         reader.U64() ==
             static_cast<uint64_t>(expected.close_yesterday_fee_per_lot) &&
         reader.ok();
}

base::Status ReadMessage(StateReader& reader,
                         trading::Message& message) noexcept {
  const auto size = reader.U64();
  if (!reader.ok() || size > trading::kMaxFrameSize) {
    return Corrupt();
  }
  const auto bytes = reader.Bytes(size);
  if (!reader.ok()) {
    return Corrupt();
  }
  return DecodeStoredStrategyReport(bytes, message);
}

bool SameSession(const ArchiveSession& a, const ArchiveSession& b) noexcept {
  if (a.client_id != b.client_id || a.session_id != b.session_id ||
      a.instrument_count != b.instrument_count) {
    return false;
  }
  for (size_t i = 0; i < a.instrument_count; ++i) {
    if (a.instruments[i].id != b.instruments[i].id ||
        a.instruments[i].price_decimals != b.instruments[i].price_decimals) {
      return false;
    }
  }
  return true;
}

bool HasInstrument(const ArchiveSession& session,
                   model::InstrumentId instrument_id) noexcept {
  for (size_t i = 0; i < session.instrument_count; ++i) {
    if (session.instruments[i].id == instrument_id) {
      return true;
    }
  }
  return false;
}
}  // namespace

struct ArchiveProjection::Storage {
  ServerConfig config;
  std::array<trading::InstrumentSpec, trading::kMaxInstruments> instruments{};
  std::array<ArchiveSession, trading::kMaxClients> sessions{};
  std::array<size_t, trading::kMaxClients> local_counts{};
  std::array<std::array<uint16_t, trading::kMaxTradingOrders>,
             trading::kMaxClients>
      local_lookup{};
  std::array<ArchiveOrder, kMaxArchiveOrders> orders{};
  size_t order_count = 0;
  std::array<uint64_t, trading::kMaxTradingFills> fill_sources{};
  uint64_t last_namespace = 0;
  ArchiveOrderUpdate update;
  bool has_update = false;
  bool restored = false;

  // Caller validates slot/local and capacity before this infallible insertion.
  ArchiveOrder& Insert(size_t slot, size_t local) noexcept {
    auto& key = local_lookup[slot][local];
    if (key == 0) {
      key = static_cast<uint16_t>(++order_count);
      auto& order = orders[key - 1];
      order.client_id = sessions[slot].client_id;
      order.session_id = sessions[slot].session_id;
      order.local_order_id = local + 1;
      local_counts[slot] = std::max(local_counts[slot], local + 1);
    }
    return orders[key - 1];
  }
};

ArchiveProjection::ArchiveProjection(const base::MonotonicClock* clock) noexcept
    : trading_(clock, &instruments_, &replay_adapter_) {}
ArchiveProjection::~ArchiveProjection() noexcept = default;

base::Status ArchiveProjection::Configure(const ServerConfig& config) noexcept {
  if (storage_ != nullptr) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (config.max_clients == 0 || config.max_clients > trading::kMaxClients ||
      !config.persistence.enabled() ||
      config.persistence.account_key[0] == '\0') {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  auto candidate = std::unique_ptr<Storage>(new (std::nothrow) Storage{});
  if (!candidate) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  auto status = instruments_.Configure(config.market_data);
  if (!status.ok()) {
    return status;
  }
  status = trading_.Init(config.trading, config.market_data);
  if (!status.ok()) {
    instruments_.Reset();
    return status;
  }
  candidate->config = config;
  for (size_t i = 0; i < config.market_data.instrument_count; ++i) {
    const auto& definition = config.market_data.instruments[i].instrument;
    candidate->instruments[i] = {definition.id, definition.price_decimals};
  }
  for (size_t i = 0; i < trading::kMaxClients; ++i) {
    candidate->sessions[i].slot = i;
  }
  std::memcpy(account_key_, config.persistence.account_key,
              sizeof(account_key_));
  storage_ = std::move(candidate);
  return {};
}

base::Status ArchiveProjection::LoadIdentity(
    std::span<const std::byte> identity) noexcept {
  if (!storage_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& s = *storage_;
  StateReader reader(identity);
  if (reader.U64() != 2) {
    return Corrupt();
  }
  char key[kMaxAccountKeyBytes + 1]{};
  if (!reader.Text(key, sizeof(key)) || std::strcmp(key, account_key_) != 0) {
    return Corrupt();
  }
  const std::array<uint64_t, 2> ledger{reader.U64(), reader.U64()};
  const auto last_namespace = reader.U64();
  const auto order_highwater = reader.U64();
  const auto cancel_highwater = reader.U64();
  if ((ledger[0] | ledger[1]) == 0 || last_namespace == 0 ||
      order_highwater == 0 || cancel_highwater == 0 ||
      ((ledger_id_[0] | ledger_id_[1]) != 0 && ledger != ledger_id_) ||
      last_namespace < s.last_namespace ||
      reader.U64() != s.config.market_data.instrument_count) {
    return Corrupt();
  }
  for (size_t i = 0; i < s.config.market_data.instrument_count; ++i) {
    if (!MatchInstrument(reader,
                         s.config.market_data.instruments[i].instrument)) {
      return Corrupt();
    }
  }
  if (reader.U64() != static_cast<unsigned>(s.config.trading.mode) ||
      reader.U64() != s.config.trading.report_interval_ns) {
    return Corrupt();
  }
  std::array<ArchiveSession, trading::kMaxClients> sessions{};
  for (size_t i = 0; i < sessions.size(); ++i) {
    auto& session = sessions[i];
    session.slot = i;
    session.client_id = reader.U64();
    session.session_id = reader.U64();
    session.instrument_count = reader.U64();
    if (session.instrument_count > trading::kMaxInstruments ||
        ((session.client_id == 0) != (session.session_id == 0)) ||
        session.session_id > last_namespace ||
        (session.client_id != 0 && i >= s.config.max_clients) ||
        ((session.client_id == 0) != (session.instrument_count == 0))) {
      return Corrupt();
    }
    for (size_t j = 0; j < session.instrument_count; ++j) {
      const auto id = reader.U64();
      const auto decimals = reader.U64();
      model::InstrumentDefinition known;
      if (decimals > UINT8_MAX || !instruments_.TryGetInstrument({id}, known) ||
          known.price_decimals != decimals) {
        return Corrupt();
      }
      session.instruments[j] = {{id}, static_cast<uint8_t>(decimals)};
      for (size_t k = 0; k < j; ++k) {
        if (session.instruments[k].id.value == id) {
          return Corrupt();
        }
      }
    }
    if (s.sessions[i].client_id != 0 && !SameSession(s.sessions[i], session)) {
      return Corrupt();
    }
    if (session.client_id != 0) {
      for (size_t j = 0; j < i; ++j) {
        if (sessions[j].client_id == session.client_id ||
            sessions[j].session_id == session.session_id) {
          return Corrupt();
        }
      }
    }
  }
  if (!reader.done()) {
    return Corrupt();
  }
  s.sessions = sessions;
  s.last_namespace = last_namespace;
  ledger_id_ = ledger;
  return {};
}

base::Status ArchiveProjection::Restore(
    std::span<const std::byte> archive_baseline) noexcept {
  if (!storage_ || storage_->restored) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  StateReader reader(archive_baseline);
  if (reader.U64() != kArchiveFormatVersion) {
    return Corrupt();
  }
  const std::array<uint64_t, 2> ledger{reader.U64(), reader.U64()};
  const auto run = reader.U64();
  char key[kMaxAccountKeyBytes + 1]{};
  if ((ledger[0] | ledger[1]) == 0 || run == 0 ||
      !reader.Text(key, sizeof(key)) || std::strcmp(key, account_key_) != 0) {
    return Corrupt();
  }
  const auto metadata_size = reader.U64();
  const auto metadata = reader.Bytes(metadata_size);
  const auto checkpoint_size = reader.U64();
  const auto checkpoint = reader.Bytes(checkpoint_size);
  if (!reader.done()) {
    return Corrupt();
  }
  auto status = LoadIdentity(metadata);
  if (!status.ok()) {
    return status;
  }
  if (ledger_id_ != ledger || run > storage_->last_namespace) {
    return Corrupt();
  }
  run_id_ = run;
  status = RestoreGateway(checkpoint);
  if (!status.ok()) {
    return status;
  }
  storage_->restored = true;
  return {};
}

base::Status ArchiveProjection::RestoreGateway(
    std::span<const std::byte> checkpoint) noexcept {
  auto& s = *storage_;
  StateReader reader(checkpoint);
  if (reader.U64() != 1) {
    return Corrupt();
  }
  for (const auto id : ledger_id_) {
    if (reader.U64() != id) {
      return Corrupt();
    }
  }
  if (reader.U64() != run_id_) {
    return Corrupt();
  }
  // Archive baselines represent the new run before LSN 1. The cursor fields
  // describe the gateway's own WAL checkpoint and are not SQL progress.
  const auto covered = reader.U64();
  (void)reader.U64();
  (void)reader.U64();
  last_record_time_ = reader.U64();
  if (covered != 0) {
    return Corrupt();
  }
  for (size_t slot = 0; slot < trading::kMaxClients; ++slot) {
    const auto id = reader.U64();
    const auto session = reader.U64();
    const auto count = reader.U64();
    (void)reader.U64();  // Last allocated local cancel ID.
    if (!reader.ok() || count > trading::kMaxTradingOrders ||
        ((id == 0) != (session == 0)) || (id == 0 && count != 0) ||
        (id != 0 && (id != s.sessions[slot].client_id ||
                     session != s.sessions[slot].session_id))) {
      return Corrupt();
    }
    for (size_t local = 0; local < count; ++local) {
      auto& order = s.Insert(slot, local);
      order.core_id = {reader.U64()};
      order.report_sequence = reader.U64();
      order.core_cancel_id = {reader.U64()};
      order.local_cancel_id = {reader.U64()};
      (void)reader.U64();  // Gateway retry deadline has no archive semantics.
      if (order.core_id.value == 0) {
        trading::Message message;
        const auto status = ReadMessage(reader, message);
        if (!status.ok() ||
            message.type != trading::MessageType::kOrderUpdate ||
            message.update.order.strategy_order_id.value != local + 1 ||
            message.update.order.state != model::OrderState::kRejected ||
            !HasInstrument(s.sessions[slot],
                           message.update.order.instrument_id)) {
          return Corrupt();
        }
        order.snapshot = message.update.order;
      }
    }
  }
  const auto ledger_size = reader.U64();
  const auto ledger = reader.Bytes(ledger_size);
  const auto mock_size = reader.U64();
  (void)reader.Bytes(mock_size);
  if (!reader.done()) {
    return Corrupt();
  }
  auto status = trading_.RestoreCheckpoint(ledger);
  if (!status.ok()) {
    return status;
  }
  return RefreshOrders();
}

base::Status ArchiveProjection::RefreshOrders() noexcept {
  auto& s = *storage_;
  size_t core_count = 0;
  for (size_t i = 0; i < s.order_count; ++i) {
    auto& order = s.orders[i];
    if (order.core_id.value == 0) {
      continue;
    }
    if (!trading_.TryGetOrder(order.core_id, order.snapshot)) {
      return Corrupt();
    }
    size_t slot = 0;
    while (slot < s.sessions.size() &&
           s.sessions[slot].session_id != order.session_id) {
      ++slot;
    }
    if (slot == s.sessions.size() ||
        !HasInstrument(s.sessions[slot], order.snapshot.instrument_id)) {
      return Corrupt();
    }
    ++core_count;
    for (size_t j = 0; j < i; ++j) {
      if (s.orders[j].core_id == order.core_id) {
        return Corrupt();
      }
    }
  }
  return core_count == trading_.order_count() ? base::Status{} : Corrupt();
}

base::Status ArchiveProjection::Replay(
    uint16_t type, uint64_t lsn, std::span<const std::byte> payload) noexcept {
  if (!storage_ || !storage_->restored) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& s = *storage_;
  s.has_update = false;
  if (last_lsn_ == UINT64_MAX || lsn != last_lsn_ + 1) {
    return Corrupt();
  }
  if (type == kMockRecord) {
    trading::MockTradingMutation mutation;
    auto status = trading::DecodeMockTradingMutation(payload, mutation);
    if (!status.ok()) {
      return status;
    }
    last_record_time_ = mutation.now;
    last_lsn_ = lsn;
    return {};
  }
  if (type != kLedgerRecord && type != kRejectedRecord) {
    return Corrupt();
  }
  StateReader reader(payload);
  const auto slot = reader.U64();
  const auto local = reader.U64();
  if (!reader.ok() || slot >= s.config.max_clients ||
      local >= trading::kMaxTradingOrders || s.sessions[slot].client_id == 0 ||
      local > s.local_counts[slot]) {
    return Corrupt();
  }
  auto& key = s.local_lookup[slot][local];
  if (type == kRejectedRecord) {
    trading::Message message;
    auto status = ReadMessage(reader, message);
    if (!status.ok() || !reader.done() ||
        message.type != trading::MessageType::kOrderUpdate ||
        message.update.order.strategy_order_id.value != local + 1 ||
        message.has_fill ||
        !HasInstrument(s.sessions[slot], message.update.order.instrument_id) ||
        (message.update.kind != model::OrderUpdateKind::kRejected &&
         message.update.kind != model::OrderUpdateKind::kCancelRejected) ||
        (key == 0 &&
         (message.update.kind != model::OrderUpdateKind::kRejected ||
          message.update.order.state != model::OrderState::kRejected))) {
      return Corrupt();
    }
    auto& order = s.Insert(slot, local);
    if (message.update.order.report_sequence != order.report_sequence + 1) {
      return Corrupt();
    }
    if (order.core_id.value == 0) {
      order.snapshot = message.update.order;
    }
    order.report_sequence = message.update.order.report_sequence;
    order.local_cancel_id = message.update.cancel_request_id;
    order.core_cancel_id = order.snapshot.cancel_request_id;
    order.source_lsn = lsn;
    s.update = {order.client_id, order.session_id,     order.local_order_id,
                message.update,  message.notify_order, message.has_fill,
                message.fill};
    s.has_update = true;
    last_record_time_ = message.update.order.receive_time_ns;
    last_lsn_ = lsn;
    return {};
  }
  const auto local_cancel = reader.U64();
  if (!reader.ok() || payload.size() < 24) {
    return Corrupt();
  }
  const auto encoded = reader.Bytes(payload.size() - 24);
  if (!reader.done()) {
    return Corrupt();
  }
  trading::TradingMutation mutation;
  auto status = trading::DecodeTradingMutation(encoded, mutation);
  if (!status.ok()) {
    return status;
  }
  using Kind = trading::TradingMutationKind;
  if (mutation.kind == Kind::kNewOrder) {
    if (key != 0 || local != s.local_counts[slot] ||
        FindOrder(mutation.client_order_id) != nullptr ||
        !HasInstrument(s.sessions[slot], mutation.request.instrument_id)) {
      return Corrupt();
    }
  } else if (key == 0 ||
             s.orders[key - 1].core_id != mutation.client_order_id) {
    return Corrupt();
  }
  const auto previous_fill_count = trading_.fill_count();
  trading::TradingDispatch dispatch;
  status = trading_.ReplayMutation(mutation, &dispatch);
  if (!status.ok()) {
    return status;
  }
  auto& order = s.Insert(slot, local);
  if (mutation.kind == Kind::kNewOrder) {
    order.core_id = mutation.client_order_id;
  }
  if (mutation.kind == Kind::kCancel) {
    order.local_cancel_id = {local_cancel};
    order.core_cancel_id = mutation.cancel_id;
  }
  if (!trading_.TryGetOrder(order.core_id, order.snapshot)) {
    return Corrupt();
  }
  order.source_lsn = lsn;
  if (dispatch.update.order.client_order_id.value != 0) {
    ++order.report_sequence;
    dispatch.update.order.strategy_order_id = {local + 1};
    dispatch.update.order.report_sequence = order.report_sequence;
    if (dispatch.update.order.cancel_request_id == order.core_cancel_id) {
      dispatch.update.order.cancel_request_id = order.local_cancel_id;
    } else {
      dispatch.update.order.cancel_request_id = {};
    }
    const bool cancel_report =
        dispatch.update.kind == model::OrderUpdateKind::kCanceled ||
        dispatch.update.kind == model::OrderUpdateKind::kCancelRejected ||
        dispatch.update.kind == model::OrderUpdateKind::kCancelNotSent;
    dispatch.update.cancel_request_id =
        cancel_report &&
                dispatch.update.cancel_request_id == order.core_cancel_id
            ? order.local_cancel_id
            : model::CancelRequestId{};
    if (dispatch.has_fill) {
      dispatch.fill.strategy_order_id = {local + 1};
    }
    s.update = {order.client_id, order.session_id,      order.local_order_id,
                dispatch.update, dispatch.notify_order, dispatch.has_fill,
                dispatch.fill};
    s.has_update = true;
  }
  for (size_t i = previous_fill_count; i < trading_.fill_count(); ++i) {
    s.fill_sources[i] = lsn;
  }
  last_record_time_ = mutation.now;
  last_lsn_ = lsn;
  return {};
}

std::span<const ArchiveSession> ArchiveProjection::sessions() const noexcept {
  return storage_ ? std::span<const ArchiveSession>(storage_->sessions)
                  : std::span<const ArchiveSession>{};
}
std::span<const trading::InstrumentSpec> ArchiveProjection::instruments()
    const noexcept {
  return storage_
             ? std::span<const trading::InstrumentSpec>(storage_->instruments)
                   .first(storage_->config.market_data.instrument_count)
             : std::span<const trading::InstrumentSpec>{};
}
std::span<const ArchiveOrder> ArchiveProjection::orders() const noexcept {
  return storage_ ? std::span(storage_->orders).first(storage_->order_count)
                  : std::span<const ArchiveOrder>{};
}
const ArchiveOrder* ArchiveProjection::FindOrder(
    model::ClientOrderId core_id) const noexcept {
  if (core_id.value == 0) {
    return nullptr;
  }
  for (const auto& order : orders()) {
    if (order.core_id == core_id) {
      return &order;
    }
  }
  return nullptr;
}
uint64_t ArchiveProjection::FillSourceLsn(size_t index) const noexcept {
  return storage_ && index < trading_.fill_count()
             ? storage_->fill_sources[index]
             : 0;
}
bool ArchiveProjection::has_update() const noexcept {
  return storage_ && storage_->has_update;
}
const ArchiveOrderUpdate& ArchiveProjection::last_update() const noexcept {
  return storage_->update;
}

}  // namespace yquant::tradingd
