#include "yquant/trading/trading_persistence.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

#include "trading/trading_storage.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
constexpr uint64_t kCheckpointMagic = 0x3154504344515955ULL;
constexpr uint64_t kMutationVersion = 2;
constexpr uint64_t kCheckpointVersion = 2;
constexpr size_t kMaxReasonBytes = 128;

bool IsFuturesInstrument(model::InstrumentId id) noexcept {
  return model::IsFuturesVenue(static_cast<model::Venue>(id.value >> 32));
}

// Bounds checked field codecs; truncated input never accesses outside its span.
class Writer {
 public:
  explicit Writer(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}
  void Uint(uint64_t value, size_t width = 8) noexcept {
    if (!Reserve(width)) {
      return;
    }
    for (size_t index = 0; index < width; ++index) {
      bytes_[size_++] = static_cast<std::byte>(value & 0xffU);
      value >>= 8;
    }
  }
  void Signed(int64_t value) noexcept { Uint(std::bit_cast<uint64_t>(value)); }
  void Bytes(std::string_view value) noexcept {
    if (!Reserve(value.size())) {
      return;
    }
    if (!value.empty()) {
      std::memcpy(bytes_.data() + size_, value.data(), value.size());
    }
    size_ += value.size();
  }
  bool ok() const noexcept { return ok_; }
  size_t size() const noexcept { return size_; }
  void Reject() noexcept { ok_ = false; }

 private:
  bool Reserve(size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) {
      return ok_ = false;
    }
    return true;
  }
  std::span<std::byte> bytes_;
  size_t size_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
  uint64_t Uint(size_t width = 8) noexcept {
    if (!Reserve(width)) {
      return 0;
    }
    uint64_t value = 0;
    for (size_t index = 0; index < width; ++index) {
      value |= static_cast<uint64_t>(bytes_[size_++]) << (8 * index);
    }
    return value;
  }
  int64_t Signed() noexcept { return std::bit_cast<int64_t>(Uint()); }
  bool Boolean() noexcept {
    const auto value = Uint(1);
    if (value > 1) {
      Reject();
    }
    return value == 1;
  }
  std::string_view Bytes(size_t size) noexcept {
    if (!Reserve(size)) {
      return {};
    }
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + size_);
    size_ += size;
    return {begin, size};
  }
  bool ok() const noexcept { return ok_; }
  bool done() const noexcept { return ok_ && size_ == bytes_.size(); }
  void Reject() noexcept { ok_ = false; }

 private:
  bool Reserve(size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) {
      return ok_ = false;
    }
    return true;
  }
  std::span<const std::byte> bytes_;
  size_t size_ = 0;
  bool ok_ = true;
};

template <typename Id>
void WriteId(Writer& writer, const Id& id) noexcept {
  writer.Uint(id.view().size(), 1);
  writer.Bytes(id.view());
}
template <typename Id>
void ReadId(Reader& reader, Id& id) noexcept {
  const auto size = reader.Uint(1);
  const auto bytes = reader.Bytes(size);
  if (reader.ok() && size != 0 && id.Assign(bytes) != ErrorCode::kOk) {
    reader.Reject();
  }
}

void WriteRequest(Writer& w, const model::OrderRequest& value) noexcept {
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<uint8_t>(value.side), 1);
  w.Signed(value.limit_price);
  w.Signed(value.quantity);
}
// Reads a OrderRequest value and records malformed input in the reader.
model::OrderRequest ReadRequest(Reader& r) noexcept {
  model::OrderRequest value;
  value.instrument_id.value = r.Uint();
  value.side = static_cast<model::OrderSide>(r.Uint(1));
  value.limit_price = r.Signed();
  value.quantity = r.Signed();
  return value;
}
void WriteOrder(Writer& w, const model::OrderSnapshot& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.adapter_order_id);
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<uint8_t>(value.side), 1);
  w.Signed(value.limit_price);
  w.Signed(value.original_qty);
  w.Signed(value.cumulative_filled_qty);
  w.Signed(value.leaves_qty);
  w.Uint(static_cast<uint8_t>(value.state), 1);
  w.Uint(value.cancel_pending, 1);
  w.Uint(value.cancel_request_id.value);
  w.Uint(value.report_sequence);
  w.Uint(value.receive_time_ns);
}
// Reads a OrderSnapshot value and records malformed input in the reader.
model::OrderSnapshot ReadOrder(Reader& r) noexcept {
  model::OrderSnapshot value;
  value.client_order_id.value = r.Uint();
  ReadId(r, value.adapter_order_id);
  value.instrument_id.value = r.Uint();
  value.side = static_cast<model::OrderSide>(r.Uint(1));
  value.limit_price = r.Signed();
  value.original_qty = r.Signed();
  value.cumulative_filled_qty = r.Signed();
  value.leaves_qty = r.Signed();
  value.state = static_cast<model::OrderState>(r.Uint(1));
  value.cancel_pending = r.Boolean();
  value.cancel_request_id.value = r.Uint();
  value.report_sequence = r.Uint();
  value.receive_time_ns = r.Uint();
  return value;
}
void WriteFill(Writer& w, const model::Fill& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.adapter_order_id);
  WriteId(w, value.execution_id);
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<uint8_t>(value.side), 1);
  w.Signed(value.price);
  w.Signed(value.quantity);
  w.Uint(value.event_time_ns);
  w.Uint(value.receive_time_ns);
  w.Uint(value.has_event_time, 1);
}
// Reads a Fill value and records malformed input in the reader.
model::Fill ReadFill(Reader& r) noexcept {
  model::Fill value;
  value.client_order_id.value = r.Uint();
  ReadId(r, value.adapter_order_id);
  ReadId(r, value.execution_id);
  value.instrument_id.value = r.Uint();
  value.side = static_cast<model::OrderSide>(r.Uint(1));
  value.price = r.Signed();
  value.quantity = r.Signed();
  value.event_time_ns = r.Uint();
  value.receive_time_ns = r.Uint();
  value.has_event_time = r.Boolean();
  return value;
}
void WriteEvent(Writer& w, const model::TradingEvent& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.adapter_order_id);
  w.Uint(value.report_sequence);
  w.Uint(static_cast<uint8_t>(value.kind), 1);
  w.Signed(value.cumulative_filled_qty);
  w.Uint(value.receive_time_ns);
  WriteFill(w, value.fill);
  w.Uint(value.cancel_request_id.value);
  if (value.reason.size > kMaxReasonBytes ||
      value.reason.text[value.reason.size] != '\0') {
    w.Reject();
    return;
  }
  w.Uint(std::bit_cast<uint32_t>(value.reason.code), 4);
  w.Uint(value.reason.truncated, 1);
  w.Uint(value.reason.size, 1);
  w.Bytes(value.reason.view());
}
// Reads a TradingEvent value and records malformed input in the reader.
model::TradingEvent ReadEvent(Reader& r) noexcept {
  model::TradingEvent value;
  value.client_order_id.value = r.Uint();
  ReadId(r, value.adapter_order_id);
  value.report_sequence = r.Uint();
  value.kind = static_cast<model::OrderUpdateKind>(r.Uint(1));
  value.cumulative_filled_qty = r.Signed();
  value.receive_time_ns = r.Uint();
  value.fill = ReadFill(r);
  value.cancel_request_id.value = r.Uint();
  value.reason.code = std::bit_cast<int32_t>(static_cast<uint32_t>(r.Uint(4)));
  value.reason.truncated = r.Boolean();
  const auto size = r.Uint(1);
  if (size > kMaxReasonBytes) {
    r.Reject();
    return value;
  }
  const auto bytes = r.Bytes(size);
  if (!r.ok()) {
    return value;
  }
  value.reason.size = static_cast<uint8_t>(size);
  if (size != 0) {
    std::memcpy(value.reason.text, bytes.data(), size);
  }
  value.reason.text[size] = '\0';
  return value;
}

// Encodes request execution independently of native object layout.
void WriteProgress(Writer& w, const OrderRequestProgress& progress) noexcept {
  w.Uint(static_cast<uint8_t>(progress.state), 1);
  w.Uint(progress.attempted, 1);
  w.Uint(progress.attempted_at_ns);
  w.Uint(progress.response_deadline_ns);
}
OrderRequestProgress ReadProgress(Reader& r) noexcept {
  OrderRequestProgress value;
  value.state = static_cast<OrderRequestState>(r.Uint(1));
  value.attempted = r.Boolean();
  value.attempted_at_ns = r.Uint();
  value.response_deadline_ns = r.Uint();
  return value;
}
void WriteExecution(Writer& w, const RequestExecution& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteProgress(w, value.new_request);
  w.Uint(value.cancel_request_id.value);
  WriteProgress(w, value.cancel_request);
  w.Uint(value.new_local_result_pending, 1);
  w.Uint(value.cancel_local_result_pending, 1);
}
RequestExecution ReadExecution(Reader& r) noexcept {
  RequestExecution value;
  value.client_order_id.value = r.Uint();
  value.new_request = ReadProgress(r);
  value.cancel_request_id.value = r.Uint();
  value.cancel_request = ReadProgress(r);
  value.new_local_result_pending = r.Boolean();
  value.cancel_local_result_pending = r.Boolean();
  return value;
}
void WriteExecutionRequest(Writer& w,
                           const OrderExecutionRequest& value) noexcept {
  w.Uint(static_cast<uint8_t>(value.kind), 1);
  w.Uint(value.order_index, 4);
  w.Uint(value.client_order_id.value);
  w.Uint(value.cancel_request_id.value);
}
OrderExecutionRequest ReadExecutionRequest(Reader& r) noexcept {
  OrderExecutionRequest value;
  value.kind = static_cast<OrderRequestKind>(r.Uint(1));
  value.order_index = r.Uint(4);
  value.client_order_id.value = r.Uint();
  value.cancel_request_id.value = r.Uint();
  return value;
}
// Checks position units and uniqueness without changing the candidate.
template <typename Storage>
bool ValidatePositions(const Storage& s,
                       const md::InstrumentScope& market) noexcept {
  for (size_t i = 0; i < s.position_count; ++i) {
    const auto& p = s.positions[i];
    model::InstrumentDefinition instrument;
    if (!market.TryGetInstrument(p.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kEquity ||
        instrument.quantity_step <= 0 ||
        p.total_qty % instrument.quantity_step != 0 ||
        p.available_qty % instrument.quantity_step != 0 ||
        p.reserved_qty % instrument.quantity_step != 0 || p.total_qty < 0 ||
        p.available_qty < 0 || p.reserved_qty < 0 ||
        p.reserved_qty > p.total_qty ||
        p.available_qty > p.total_qty - p.reserved_qty) {
      return false;
    }
    for (size_t j = 0; j < i; ++j) {
      if (s.positions[j].instrument_id == p.instrument_id) {
        return false;
      }
    }
  }
  return true;
}

// Rebuilds order lookups and verifies aggregate reservations and activity.
// Only a private restore candidate may be passed; failure may leave it changed.
template <typename Storage>
bool ValidateOrders(Storage& s, const md::InstrumentScope& market) noexcept {
  constexpr auto kMaximum = std::numeric_limits<int64_t>::max();
  std::array<int64_t, md::kMaxMarketDataInstruments> quantities = {};
  int64_t reserved_cash = 0;
  size_t active = 0;
  size_t pending_cancel = 0;
  uint64_t previous_id = 0;
  for (size_t i = 0; i < s.order_count; ++i) {
    const auto& record = s.orders[i];
    const auto& o = record.core.snapshot();
    model::InstrumentDefinition instrument;
    if (o.strategy_order_id.value != 0 ||
        o.client_order_id.value <= previous_id ||
        o.client_order_id.value >= s.next_order_id ||
        !market.TryGetInstrument(o.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kEquity ||
        instrument.price_decimals > model::kMaxPriceDecimals ||
        instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
        o.position_effect != model::PositionEffect::kNone ||
        o.limit_price % instrument.price_tick != 0 ||
        o.original_qty % instrument.quantity_step != 0 ||
        o.cumulative_filled_qty % instrument.quantity_step != 0 ||
        record.futures_reservation.quantity != 0 ||
        record.futures_reservation.cash != 0 ||
        record.position_index >= s.position_count ||
        s.positions[record.position_index].instrument_id != o.instrument_id ||
        (o.side != model::OrderSide::kBuy &&
         o.side != model::OrderSide::kSell) ||
        o.limit_price <= 0 || o.original_qty <= 0 ||
        o.cumulative_filled_qty < 0 ||
        o.cumulative_filled_qty > o.original_qty ||
        static_cast<uint8_t>(o.state) >
            static_cast<uint8_t>(model::OrderState::kRejected) ||
        o.cancel_request_id.value > s.last_cancel_id) {
      return false;
    }
    previous_id = o.client_order_id.value;
    auto order_slot = TradingOrderHash(o.client_order_id);
    while (s.order_lookup[order_slot] != 0) {
      order_slot = (order_slot + 1) & (kOrderLookupCapacity - 1);
    }
    s.order_lookup[order_slot] = static_cast<uint16_t>(i + 1);
    int64_t scale = 1;
    for (size_t digit = instrument.price_decimals; digit < 6; ++digit) {
      scale *= 10;
    }
    if (record.price_scale != scale ||
        o.limit_price > kMaximum / o.original_qty ||
        o.limit_price * o.original_qty > kMaximum / scale) {
      return false;
    }
    const bool terminal = model::IsTerminalOrderState(o.state);
    const auto expected_cash = !terminal && o.side == model::OrderSide::kBuy
                                   ? o.limit_price * o.leaves_qty * scale
                                   : 0;
    const auto expected_qty =
        !terminal && o.side == model::OrderSide::kSell ? o.leaves_qty : 0;
    if (record.reserved_cash != expected_cash ||
        record.reserved_qty != expected_qty ||
        record.reserved_cash > kMaximum - reserved_cash ||
        record.reserved_qty > kMaximum - quantities[record.position_index]) {
      return false;
    }
    reserved_cash += record.reserved_cash;
    quantities[record.position_index] += record.reserved_qty;
    active += !terminal;
    pending_cancel += o.cancel_pending;
    if (!o.adapter_order_id.empty()) {
      auto slot = TradingBrokerHash(o.adapter_order_id);
      while (s.broker_lookup[slot] != 0) {
        if (s.orders[s.broker_lookup[slot] - 1]
                .core.snapshot()
                .adapter_order_id == o.adapter_order_id) {
          return false;
        }
        slot = (slot + 1) & (kBrokerLookupCapacity - 1);
      }
      s.broker_lookup[slot] = static_cast<uint16_t>(i + 1);
    }
  }
  if (active != s.active_count || pending_cancel != s.pending_cancel_count ||
      reserved_cash != s.cash.reserved) {
    return false;
  }
  for (size_t i = 0; i < s.position_count; ++i) {
    if (quantities[i] != s.positions[i].reserved_qty) {
      return false;
    }
  }
  return true;
}

// Rebuilds execution deduplication and reconciles each order's filled quantity.
template <typename Storage>
bool ValidateExecutions(Storage& s,
                        const md::InstrumentScope& market) noexcept {
  std::array<int64_t, kMaxTradingOrders> filled = {};
  for (size_t i = 0; i < s.fill_count; ++i) {
    const auto& f = s.fills[i];
    size_t index = 0;
    while (index < s.order_count &&
           s.orders[index].core.snapshot().client_order_id !=
               f.client_order_id) {
      ++index;
    }
    if (index == s.order_count) {
      return false;
    }
    const auto& o = s.orders[index].core.snapshot();
    model::InstrumentDefinition instrument;
    if (f.strategy_order_id.value != 0 ||
        !market.TryGetInstrument(f.instrument_id, instrument) ||
        instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
        f.position_effect != model::PositionEffect::kNone ||
        f.price % instrument.price_tick != 0 ||
        f.quantity % instrument.quantity_step != 0 ||
        f.adapter_order_id.empty() || f.execution_id.empty() ||
        f.adapter_order_id != o.adapter_order_id ||
        f.instrument_id != o.instrument_id || f.side != o.side ||
        f.price <= 0 || f.quantity <= 0 ||
        (!f.has_event_time && f.event_time_ns != 0) ||
        (o.side == model::OrderSide::kBuy && f.price > o.limit_price) ||
        (o.side == model::OrderSide::kSell && f.price < o.limit_price) ||
        f.quantity > o.cumulative_filled_qty - filled[index]) {
      return false;
    }
    filled[index] += f.quantity;
    auto slot = TradingFillHash(f.client_order_id, f.execution_id);
    while (s.fill_lookup[slot] != 0) {
      const auto& prior = s.fills[s.fill_lookup[slot] - 1];
      if (prior.client_order_id == f.client_order_id &&
          prior.execution_id == f.execution_id) {
        return false;
      }
      slot = (slot + 1) & (kFillLookupCapacity - 1);
    }
    s.fill_lookup[slot] = static_cast<uint16_t>(i + 1);
  }
  for (size_t i = 0; i < s.order_count; ++i) {
    if (filled[i] != s.orders[i].core.snapshot().cumulative_filled_qty) {
      return false;
    }
  }
  return true;
}

// Validates a private cash checkpoint before publishing it to the engine.
template <typename Storage>
bool Validate(Storage& s, const md::InstrumentScope& market) noexcept {
  if (s.type != model::AccountType::kCash || s.next_order_id == 0 ||
      s.cash.balance < 0 || s.cash.reserved < 0 ||
      s.cash.reserved > s.cash.balance ||
      s.cash.available != s.cash.balance - s.cash.reserved ||
      s.scan_cursor > s.order_count) {
    return false;
  }
  return ValidatePositions(s, market) && ValidateOrders(s, market) &&
         ValidateExecutions(s, market) && true;
}
}  // namespace

base::Status EncodeTradingMutation(const TradingMutation& value,
                                   std::span<std::byte> bytes,
                                   size_t& size) noexcept {
  // Version 1 represents cash equities, whose position effect is implicit.
  // Reject unsupported active fields before writing instead of dropping them.
  if (!IsValidTradingMutationKind(value.kind) || value.revision == 0 ||
      value.client_order_id.value == 0 ||
      (value.kind == TradingMutationKind::kNewOrder &&
       (value.request.position_effect != model::PositionEffect::kNone ||
        IsFuturesInstrument(value.request.instrument_id))) ||
      (value.kind == TradingMutationKind::kReport &&
       (value.event.fill.strategy_order_id.value != 0 ||
        value.event.fill.position_effect != model::PositionEffect::kNone ||
        IsFuturesInstrument(value.event.fill.instrument_id)))) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  if (!IsValidTradingMutation(value) ||
      (value.format_version != 1 && value.format_version != kMutationVersion) ||
      (value.format_version == 1 &&
       (value.kind == TradingMutationKind::kCancelNotSent ||
        value.kind == TradingMutationKind::kRequestUncertain)) ||
      (value.kind == TradingMutationKind::kRequestUncertain &&
       !IsValidTradingExecutionFailure(value.failure_code))) {
    return {ErrorCode::kInvalidArgument, {}};
  }
  Writer w(bytes);
  w.Uint(value.format_version, 2);
  w.Uint(static_cast<uint8_t>(value.kind), 1);
  w.Uint(value.revision);
  w.Uint(value.now);
  w.Uint(value.client_order_id.value);
  w.Uint(value.cancel_id.value);
  switch (value.kind) {
    case TradingMutationKind::kNewOrder:
      WriteRequest(w, value.request);
      break;
    case TradingMutationKind::kReport:
      WriteEvent(w, value.event);
      break;
    case TradingMutationKind::kRequestUncertain:
      w.Uint(static_cast<uint8_t>(value.failure_code), 1);
      break;
    case TradingMutationKind::kCancelNotSent:
    case TradingMutationKind::kCancel:
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel:
    case TradingMutationKind::kLocalCancel:
      break;
  }
  if (!w.ok()) {
    return {ErrorCode::kResourceExhausted, {}};
  }
  size = w.size();
  return {};
}

base::Status DecodeTradingMutation(std::span<const std::byte> bytes,
                                   TradingMutation& out) noexcept {
  Reader r(bytes);
  const auto version = r.Uint(2);
  if (version != 1 && version != kMutationVersion) {
    return {ErrorCode::kCorrupt, {}};
  }
  TradingMutation value;
  value.format_version = static_cast<uint16_t>(version);
  value.kind = static_cast<TradingMutationKind>(r.Uint(1));
  if (!IsValidTradingMutationKind(value.kind)) {
    return {ErrorCode::kCorrupt, {}};
  }
  value.revision = r.Uint();
  value.now = r.Uint();
  value.client_order_id.value = r.Uint();
  value.cancel_id.value = r.Uint();
  switch (value.kind) {
    case TradingMutationKind::kNewOrder:
      value.request = ReadRequest(r);
      break;
    case TradingMutationKind::kReport:
      value.event = ReadEvent(r);
      break;
    case TradingMutationKind::kRequestUncertain:
      value.failure_code = static_cast<ErrorCode>(r.Uint(1));
      break;
    case TradingMutationKind::kCancelNotSent:
    case TradingMutationKind::kCancel:
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel:
    case TradingMutationKind::kLocalCancel:
      break;
  }
  if (!IsValidTradingMutation(value) ||
      (version == 1 &&
       (value.kind == TradingMutationKind::kCancelNotSent ||
        value.kind == TradingMutationKind::kRequestUncertain)) ||
      (value.kind == TradingMutationKind::kRequestUncertain &&
       !IsValidTradingExecutionFailure(value.failure_code)) ||
      !r.done() || value.revision == 0 || value.client_order_id.value == 0 ||
      (value.kind == TradingMutationKind::kNewOrder &&
       IsFuturesInstrument(value.request.instrument_id)) ||
      (value.kind == TradingMutationKind::kReport &&
       IsFuturesInstrument(value.event.fill.instrument_id))) {
    return {ErrorCode::kCorrupt, {}};
  }
  out = value;
  return {};
}

base::Status TradingEngine::ExportCheckpoint(std::span<std::byte> bytes,
                                             size_t& size) const noexcept {
  if (storage_ == nullptr || accept_requests_) {
    return {ErrorCode::kInvalidState, {}};
  }
  const auto& s = *storage_;
  if (s.type != model::AccountType::kCash) {
    return {ErrorCode::kUnsupported, {}};
  }
  for (size_t i = 0; i < s.order_count; ++i) {
    if (s.orders[i].core.snapshot().position_effect !=
        model::PositionEffect::kNone) {
      return {ErrorCode::kUnsupported, {}};
    }
  }
  for (size_t i = 0; i < s.fill_count; ++i) {
    if (s.fills[i].position_effect != model::PositionEffect::kNone) {
      return {ErrorCode::kUnsupported, {}};
    }
  }
  Writer w(bytes);
  w.Uint(kCheckpointMagic);
  w.Uint(kCheckpointVersion, 4);
  w.Uint(s.mutation_revision);
  w.Uint(s.next_order_id);
  w.Uint(s.last_cancel_id);
  w.Signed(s.cash.balance);
  w.Signed(s.cash.reserved);
  w.Signed(s.cash.available);
  w.Uint(s.position_count, 4);
  for (size_t i = 0; i < s.position_count; ++i) {
    const auto& p = s.positions[i];
    w.Uint(p.instrument_id.value);
    w.Signed(p.total_qty);
    w.Signed(p.available_qty);
    w.Signed(p.reserved_qty);
  }
  w.Uint(s.order_count, 4);
  w.Uint(s.fill_count, 4);
  w.Uint(s.active_count, 4);
  w.Uint(s.pending_cancel_count, 4);
  w.Uint(s.scan_cursor, 4);
  w.Uint(s.scan_pending, 1);
  w.Uint(s.rescan_required, 1);
  w.Uint(s.next_retry.has_value(), 1);
  w.Uint(s.next_retry.value_or(0));
  const auto& execution = s.executor.state();
  w.Uint(s.format_version, 2);
  w.Uint(execution.response_timeout_ns);
  for (size_t i = 0; i < s.order_count; ++i) {
    const auto& record = s.orders[i];
    WriteOrder(w, record.core.snapshot());
    w.Signed(record.core.rules().price_tick);
    w.Signed(record.core.rules().quantity_step);
    w.Uint(record.core.rules().allow_unsolicited_cancel, 1);
    WriteExecution(w, execution.executions[i]);
    w.Uint(record.retry_not_before_ns);
    w.Uint(record.position_index, 4);
    w.Signed(record.price_scale);
    w.Signed(record.reserved_cash);
    w.Signed(record.reserved_qty);
  }
  for (size_t i = 0; i < s.fill_count; ++i) {
    WriteFill(w, s.fills[i]);
  }
  w.Uint(execution.request_count, 4);
  for (size_t i = 0; i < execution.request_count; ++i) {
    WriteExecutionRequest(
        w,
        execution.requests[(execution.request_head + i) % kMaxTradingRequests]);
  }
  w.Uint(execution.local_count, 4);
  for (size_t i = 0; i < execution.local_count; ++i) {
    const auto& local =
        execution
            .local_results[(execution.local_head + i) % kMaxLocalOrderUpdates];
    w.Uint(static_cast<uint8_t>(local.kind), 1);
    WriteExecutionRequest(w, local.request);
    w.Uint(local.time_ns);
  }
  if (!w.ok()) {
    return {ErrorCode::kResourceExhausted, {}};
  }
  size = w.size();
  return {};
}

base::Status TradingEngine::RestoreCheckpoint(
    std::span<const std::byte> bytes) noexcept {
  if (accept_requests_ || replaying_) {
    return {
        Fail(ErrorCode::kInvalidState, "cannot restore an executing ledger"),
        {}};
  }
  if (storage_ != nullptr && storage_->type != model::AccountType::kCash) {
    return {Fail(ErrorCode::kUnsupported, "checkpoint restore requires cash"),
            {}};
  }
  if (bytes.size() > kMaxTradingCheckpointBytes) {
    return {Fail(ErrorCode::kCorrupt, "oversized ledger checkpoint"), {}};
  }
  auto candidate = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!candidate) {
    return {ErrorCode::kResourceExhausted, {}};
  }
  Reader r(bytes);
  const auto magic = r.Uint();
  const auto version = r.Uint(4);
  if (magic != kCheckpointMagic ||
      (version != 1 && version != kCheckpointVersion)) {
    return {Fail(ErrorCode::kCorrupt, "unsupported ledger checkpoint"), {}};
  }
  auto& s = *candidate;
  s.mutation_revision = r.Uint();
  s.next_order_id = r.Uint();
  s.last_cancel_id = r.Uint();
  s.cash.balance = r.Signed();
  s.cash.reserved = r.Signed();
  s.cash.available = r.Signed();
  s.position_count = r.Uint(4);
  if (s.position_count > md::kMaxMarketDataInstruments) {
    return {ErrorCode::kCorrupt, {}};
  }
  for (size_t i = 0; i < s.position_count; ++i) {
    auto& p = s.positions[i];
    p.instrument_id.value = r.Uint();
    p.total_qty = r.Signed();
    p.available_qty = r.Signed();
    p.reserved_qty = r.Signed();
  }
  s.order_count = r.Uint(4);
  s.fill_count = r.Uint(4);
  s.active_count = r.Uint(4);
  s.pending_cancel_count = r.Uint(4);
  s.scan_cursor = r.Uint(4);
  s.scan_pending = r.Boolean();
  s.rescan_required = r.Boolean();
  const bool has_retry = r.Boolean();
  const auto retry = r.Uint();
  if (has_retry) {
    s.next_retry = retry;
  } else if (retry != 0) {
    r.Reject();
  }
  if (s.order_count > kMaxTradingOrders || s.fill_count > kMaxTradingFills ||
      s.active_count > s.order_count ||
      s.pending_cancel_count > s.order_count) {
    return {ErrorCode::kCorrupt, {}};
  }
  OrderRequestExecutor::State execution;
  execution.order_count = s.order_count;
  s.format_version = version == 1 ? 1 : static_cast<uint16_t>(r.Uint(2));
  if (version >= 2) {
    execution.response_timeout_ns = r.Uint();
  }
  if (s.format_version != 1 && s.format_version != 2) {
    return {Fail(ErrorCode::kCorrupt, "invalid checkpoint rules version"), {}};
  }
  std::array<model::OrderSnapshot, kMaxTradingOrders> snapshots;
  for (size_t i = 0; i < s.order_count; ++i) {
    auto& record = s.orders[i];
    auto& snapshot = snapshots[i];
    snapshot = ReadOrder(r);
    OrderCoreRules saved_rules;
    if (version >= 2) {
      saved_rules.price_tick = r.Signed();
      saved_rules.quantity_step = r.Signed();
      saved_rules.allow_unsolicited_cancel = r.Boolean();
    }
    auto& progress = execution.executions[i];
    if (version == 1) {
      const bool sent = r.Boolean();
      const bool cancel_sent = r.Boolean();
      const bool local = r.Boolean();
      progress.client_order_id = snapshot.client_order_id;
      progress.new_request.attempted = sent;
      progress.new_local_result_pending = local;
      progress.new_request.state =
          sent ? ((snapshot.report_sequence != 0 ||
                   model::IsTerminalOrderState(snapshot.state))
                      ? OrderRequestState::kCompleted
                      : OrderRequestState::kUncertain)
          : local || model::IsTerminalOrderState(snapshot.state)
              ? OrderRequestState::kCompleted
              : OrderRequestState::kQueued;
      progress.cancel_request_id = snapshot.cancel_request_id;
      progress.cancel_request.attempted =
          cancel_sent ||
          (snapshot.cancel_request_id.value != 0 && !snapshot.cancel_pending);
      progress.cancel_request.state =
          snapshot.cancel_pending ? cancel_sent ? OrderRequestState::kUncertain
                                                : OrderRequestState::kQueued
                                  : OrderRequestState::kCompleted;
    } else {
      progress = ReadExecution(r);
    }
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(snapshot.instrument_id, instrument)) {
      return {Fail(ErrorCode::kCorrupt, "unknown checkpoint instrument"), {}};
    }
    const OrderCoreRules current_rules{instrument.price_tick,
                                       instrument.quantity_step, false};
    if (version == 1) {
      saved_rules = current_rules;
    } else if (saved_rules != current_rules) {
      return {Fail(ErrorCode::kCorrupt, "checkpoint order rules changed"), {}};
    }
    const auto restored = OrderCore::Restore(snapshot, saved_rules);
    if (!restored.ok()) {
      return {Fail(ErrorCode::kCorrupt, restored.diagnostic.text), {}};
    }
    record.core = restored.core;
    record.retry_not_before_ns = r.Uint();
    record.position_index = r.Uint(4);
    record.price_scale = r.Signed();
    record.reserved_cash = r.Signed();
    record.reserved_qty = r.Signed();
  }
  for (size_t i = 0; i < s.fill_count; ++i) {
    s.fills[i] = ReadFill(r);
  }
  execution.request_count = r.Uint(4);
  if (execution.request_count > kMaxTradingRequests) {
    return {ErrorCode::kCorrupt, {}};
  }
  for (size_t i = 0; i < execution.request_count; ++i) {
    if (version >= 2) {
      execution.requests[i] = ReadExecutionRequest(r);
    } else {
      auto& request = execution.requests[i];
      request.kind = static_cast<OrderRequestKind>(r.Uint(1));
      request.order_index = r.Uint(4);
      request.cancel_request_id.value = r.Uint();
      if (request.order_index >= s.order_count) {
        return {ErrorCode::kCorrupt, {}};
      }
      request.client_order_id = snapshots[request.order_index].client_order_id;
    }
  }
  execution.local_count = r.Uint(4);
  if (execution.local_count > kMaxLocalOrderUpdates) {
    return {ErrorCode::kCorrupt, {}};
  }
  for (size_t i = 0; i < execution.local_count; ++i) {
    auto& local = execution.local_results[i];
    if (version >= 2) {
      local.kind = static_cast<OrderLocalResultKind>(r.Uint(1));
      local.request = ReadExecutionRequest(r);
      local.time_ns = r.Uint();
    } else {
      local.request.order_index = r.Uint(4);
      if (local.request.order_index >= s.order_count) {
        return {ErrorCode::kCorrupt, {}};
      }
      local.request.client_order_id =
          snapshots[local.request.order_index].client_order_id;
    }
  }
  const auto execution_valid =
      s.executor.RestoreState(execution, {snapshots.data(), s.order_count});
  if (execution_valid != ErrorCode::kOk) {
    return {Fail(ErrorCode::kCorrupt, "invalid checkpoint request execution"),
            {}};
  }
  if (!r.done() || !Validate(s, instruments_)) {
    return {Fail(ErrorCode::kCorrupt, "invalid ledger checkpoint invariants"),
            {}};
  }
  storage_ = std::move(candidate);
  stopping_ = false;
  error_detail_[0] = '\0';
  return {};
}
base::Status TradingEngine::RestoreReconciled(
    const TradingAccountSnapshot& baseline,
    std::span<const model::OrderSnapshot> terminal_orders,
    std::span<const model::Fill> executions) noexcept {
  if (accept_requests_ || replaying_) {
    return {
        Fail(ErrorCode::kInvalidState, "cannot reconcile an executing ledger"),
        {}};
  }
  if (baseline.type != model::AccountType::kCash ||
      baseline.futures_position_count != 0 ||
      (storage_ != nullptr && storage_->type != model::AccountType::kCash)) {
    return {Fail(ErrorCode::kUnsupported, "reconciliation requires cash"), {}};
  }
  if (baseline.position_count > md::kMaxMarketDataInstruments ||
      terminal_orders.size() > kMaxTradingOrders ||
      executions.size() > kMaxTradingFills) {
    return {Fail(ErrorCode::kResourceExhausted,
                 "reconciled state exceeds capacity"),
            {}};
  }
  auto candidate = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!candidate) {
    return {ErrorCode::kResourceExhausted, {}};
  }
  auto& s = *candidate;
  s.cash = {baseline.cash_balance, 0, baseline.cash_balance};
  s.next_order_id = next_order_id();
  if (storage_ != nullptr) {
    s.last_cancel_id = storage_->last_cancel_id;
  }
  s.position_count = baseline.position_count;
  for (size_t i = 0; i < s.position_count; ++i) {
    if (baseline.positions[i].reserved_qty != 0) {
      return {
          Fail(ErrorCode::kCorrupt, "reconciled baseline retains reservations"),
          {}};
    }
    s.positions[i] = baseline.positions[i];
  }
  s.order_count = terminal_orders.size();
  for (size_t i = 0; i < s.order_count; ++i) {
    const auto& order = terminal_orders[i];
    if (order.strategy_order_id.value != 0 ||
        order.position_effect != model::PositionEffect::kNone ||
        !model::IsTerminalOrderState(order.state) || order.cancel_pending ||
        order.client_order_id.value == 0 ||
        order.client_order_id.value == UINT64_MAX) {
      return {Fail(ErrorCode::kCorrupt, "reconciled order is not a final fact"),
              {}};
    }
    auto& record = s.orders[i];

    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(order.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kEquity ||
        instrument.price_decimals > model::kMaxPriceDecimals) {
      return {Fail(ErrorCode::kCorrupt,
                   "unknown reconciled instrument or precision"),
              {}};
    }
    const auto restored = OrderCore::Restore(
        order, {instrument.price_tick, instrument.quantity_step, false});
    if (!restored.ok()) {
      return {Fail(ErrorCode::kCorrupt, restored.diagnostic.text), {}};
    }
    record.core = restored.core;
    record.price_scale = 1;
    for (size_t digit = instrument.price_decimals; digit < 6; ++digit) {
      record.price_scale *= 10;
    }
    auto position = size_t{0};
    while (position < s.position_count &&
           s.positions[position].instrument_id != order.instrument_id) {
      ++position;
    }
    if (position == s.position_count) {
      if (s.position_count == md::kMaxMarketDataInstruments) {
        return {ErrorCode::kResourceExhausted, {}};
      }
      s.positions[s.position_count++].instrument_id = order.instrument_id;
    }
    record.position_index = position;
    s.next_order_id =
        std::max(s.next_order_id, order.client_order_id.value + 1);
    s.last_cancel_id =
        std::max(s.last_cancel_id, order.cancel_request_id.value);
  }
  std::sort(s.orders.begin(), s.orders.begin() + s.order_count,
            [](const auto& left, const auto& right) noexcept {
              return left.core.snapshot().client_order_id.value <
                     right.core.snapshot().client_order_id.value;
            });
  s.fill_count = executions.size();
  for (size_t i = 0; i < s.fill_count; ++i) {
    s.fills[i] = executions[i];
  }
  OrderRequestExecutor::State execution;
  execution.order_count = s.order_count;
  std::array<model::OrderSnapshot, kMaxTradingOrders> snapshots;
  for (size_t i = 0; i < s.order_count; ++i) {
    const auto& order = s.orders[i].core.snapshot();
    snapshots[i] = order;
    auto& value = execution.executions[i];
    value.client_order_id = order.client_order_id;
    value.new_request.attempted = true;
    value.cancel_request_id = order.cancel_request_id;
    value.cancel_request.attempted = order.cancel_request_id.value != 0;
  }
  if (s.executor.RestoreState(execution, {snapshots.data(), s.order_count}) !=
      ErrorCode::kOk) {
    return {Fail(ErrorCode::kCorrupt, "invalid reconciled execution state"),
            {}};
  }
  if (!Validate(s, instruments_)) {
    return {Fail(ErrorCode::kCorrupt,
                 "invalid reconciled account or execution history"),
            {}};
  }
  storage_ = std::move(candidate);
  stopping_ = false;
  error_detail_[0] = '\0';
  return {};
}
}  // namespace yquant::trading
