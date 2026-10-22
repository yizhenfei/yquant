#include "yquant/trading/trading_persistence.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>

#include "trading/trading_storage.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::trading {
namespace {
using base::ErrorCode;
constexpr std::uint64_t kCheckpointMagic = 0x3154504344515955ULL;
constexpr std::uint64_t kMutationVersion = 1;
constexpr std::uint64_t kCheckpointVersion = 1;
constexpr std::size_t kMaxReasonBytes = 128;

bool IsFuturesInstrument(model::InstrumentId id) noexcept {
  return model::IsFuturesVenue(static_cast<model::Venue>(id.value >> 32));
}

// Bounds checked field codecs; truncated input never accesses outside its span.
class Writer {
 public:
  explicit Writer(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}
  void Uint(std::uint64_t value, std::size_t width = 8) noexcept {
    if (!Reserve(width)) return;
    for (std::size_t index = 0; index < width; ++index) {
      bytes_[size_++] = static_cast<std::byte>(value & 0xffU);
      value >>= 8;
    }
  }
  void Signed(std::int64_t value) noexcept {
    Uint(std::bit_cast<std::uint64_t>(value));
  }
  void Bytes(std::string_view value) noexcept {
    if (!Reserve(value.size())) return;
    if (!value.empty())
      std::memcpy(bytes_.data() + size_, value.data(), value.size());
    size_ += value.size();
  }
  bool ok() const noexcept { return ok_; }
  std::size_t size() const noexcept { return size_; }
  void Reject() noexcept { ok_ = false; }

 private:
  bool Reserve(std::size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) return ok_ = false;
    return true;
  }
  std::span<std::byte> bytes_;
  std::size_t size_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
  std::uint64_t Uint(std::size_t width = 8) noexcept {
    if (!Reserve(width)) return 0;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index)
      value |= static_cast<std::uint64_t>(bytes_[size_++]) << (8 * index);
    return value;
  }
  std::int64_t Signed() noexcept { return std::bit_cast<std::int64_t>(Uint()); }
  bool Boolean() noexcept {
    const auto value = Uint(1);
    if (value > 1) Reject();
    return value == 1;
  }
  std::string_view Bytes(std::size_t size) noexcept {
    if (!Reserve(size)) return {};
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + size_);
    size_ += size;
    return {begin, size};
  }
  bool ok() const noexcept { return ok_; }
  bool done() const noexcept { return ok_ && size_ == bytes_.size(); }
  void Reject() noexcept { ok_ = false; }

 private:
  bool Reserve(std::size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) return ok_ = false;
    return true;
  }
  std::span<const std::byte> bytes_;
  std::size_t size_ = 0;
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
  if (reader.ok() && size != 0 && id.Assign(bytes) != ErrorCode::kOk)
    reader.Reject();
}

void WriteRequest(Writer& w, const model::OrderRequest& value) noexcept {
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<std::uint8_t>(value.side), 1);
  w.Signed(value.limit_price);
  w.Signed(value.quantity);
}
void ReadRequest(Reader& r, model::OrderRequest& value) noexcept {
  value.instrument_id.value = r.Uint();
  value.side = static_cast<model::OrderSide>(r.Uint(1));
  value.limit_price = r.Signed();
  value.quantity = r.Signed();
}
void WriteOrder(Writer& w, const model::OrderSnapshot& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.broker_order_id);
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<std::uint8_t>(value.side), 1);
  w.Signed(value.limit_price);
  w.Signed(value.original_qty);
  w.Signed(value.cumulative_filled_qty);
  w.Signed(value.leaves_qty);
  w.Uint(static_cast<std::uint8_t>(value.state), 1);
  w.Uint(value.cancel_pending, 1);
  w.Uint(value.cancel_request_id.value);
  w.Uint(value.report_sequence);
  w.Uint(value.receive_time_ns);
}
void ReadOrder(Reader& r, model::OrderSnapshot& value) noexcept {
  value.client_order_id.value = r.Uint();
  ReadId(r, value.broker_order_id);
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
}
void WriteFill(Writer& w, const model::Fill& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.broker_order_id);
  WriteId(w, value.execution_id);
  w.Uint(value.instrument_id.value);
  w.Uint(static_cast<std::uint8_t>(value.side), 1);
  w.Signed(value.price);
  w.Signed(value.quantity);
  w.Uint(value.event_time_ns);
  w.Uint(value.receive_time_ns);
  w.Uint(value.has_event_time, 1);
}
void ReadFill(Reader& r, model::Fill& value) noexcept {
  value.client_order_id.value = r.Uint();
  ReadId(r, value.broker_order_id);
  ReadId(r, value.execution_id);
  value.instrument_id.value = r.Uint();
  value.side = static_cast<model::OrderSide>(r.Uint(1));
  value.price = r.Signed();
  value.quantity = r.Signed();
  value.event_time_ns = r.Uint();
  value.receive_time_ns = r.Uint();
  value.has_event_time = r.Boolean();
}
void WriteEvent(Writer& w, const TradingEvent& value) noexcept {
  w.Uint(value.client_order_id.value);
  WriteId(w, value.broker_order_id);
  w.Uint(value.report_sequence);
  w.Uint(static_cast<std::uint8_t>(value.kind), 1);
  w.Signed(value.cumulative_filled_qty);
  w.Uint(value.receive_time_ns);
  WriteFill(w, value.fill);
  w.Uint(value.cancel_request_id.value);
  if (value.reason.size > kMaxReasonBytes ||
      value.reason.text[value.reason.size] != '\0') {
    w.Reject();
    return;
  }
  w.Uint(std::bit_cast<std::uint32_t>(value.reason.code), 4);
  w.Uint(value.reason.truncated, 1);
  w.Uint(value.reason.size, 1);
  w.Bytes(value.reason.view());
}
void ReadEvent(Reader& r, TradingEvent& value) noexcept {
  value.client_order_id.value = r.Uint();
  ReadId(r, value.broker_order_id);
  value.report_sequence = r.Uint();
  value.kind = static_cast<model::OrderUpdateKind>(r.Uint(1));
  value.cumulative_filled_qty = r.Signed();
  value.receive_time_ns = r.Uint();
  ReadFill(r, value.fill);
  value.cancel_request_id.value = r.Uint();
  value.reason.code =
      std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(r.Uint(4)));
  value.reason.truncated = r.Boolean();
  const auto size = r.Uint(1);
  if (size > kMaxReasonBytes) {
    r.Reject();
    return;
  }
  const auto bytes = r.Bytes(size);
  if (!r.ok()) return;
  value.reason.size = static_cast<std::uint8_t>(size);
  if (size != 0) std::memcpy(value.reason.text, bytes.data(), size);
  value.reason.text[size] = '\0';
}

// Checks arithmetic and all cross-record reservations/queue identities before
// publishing a restored candidate. Its bounded scratch storage lives on stack.
template <typename Storage>
bool Validate(Storage& s, const md::InstrumentScope& market) noexcept {
  constexpr auto kMaximum = std::numeric_limits<std::int64_t>::max();
  if (s.type != model::AccountType::kCash || s.next_order_id == 0 ||
      s.cash.balance < 0 || s.cash.reserved < 0 ||
      s.cash.reserved > s.cash.balance ||
      s.cash.available != s.cash.balance - s.cash.reserved ||
      s.scan_cursor > s.order_count)
    return false;
  std::array<std::int64_t, md::kMaxMarketDataInstruments> quantities = {};
  std::array<std::int64_t, kMaxTradingOrders> filled = {};
  std::array<bool, kMaxTradingOrders> queued_new = {};
  std::array<bool, kMaxTradingOrders> queued_cancel = {};
  std::array<bool, kMaxTradingOrders> local = {};
  std::int64_t reserved_cash = 0;
  std::size_t active = 0;
  std::size_t pending_cancel = 0;
  std::uint64_t previous_id = 0;
  for (std::size_t i = 0; i < s.position_count; ++i) {
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
        p.available_qty > p.total_qty - p.reserved_qty)
      return false;
    for (std::size_t j = 0; j < i; ++j)
      if (s.positions[j].instrument_id == p.instrument_id) return false;
  }
  for (std::size_t i = 0; i < s.order_count; ++i) {
    const auto& record = s.orders[i];
    const auto& o = record.snapshot;
    model::InstrumentDefinition instrument;
    if (o.client_order_id.value <= previous_id ||
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
        static_cast<std::uint8_t>(o.state) >
            static_cast<std::uint8_t>(model::OrderState::kRejected) ||
        o.cancel_request_id.value > s.last_cancel_id)
      return false;
    previous_id = o.client_order_id.value;
    auto order_slot = TradingOrderHash(o.client_order_id);
    while (s.order_lookup[order_slot] != 0)
      order_slot = (order_slot + 1) & (kOrderLookupCapacity - 1);
    s.order_lookup[order_slot] = static_cast<std::uint16_t>(i + 1);
    std::int64_t scale = 1;
    for (std::size_t digit = instrument.price_decimals; digit < 6; ++digit)
      scale *= 10;
    if (record.price_scale != scale ||
        o.limit_price > kMaximum / o.original_qty ||
        o.limit_price * o.original_qty > kMaximum / scale)
      return false;
    const bool terminal = model::IsTerminalOrderState(o.state);
    if (o.leaves_qty !=
            (terminal ? 0 : o.original_qty - o.cumulative_filled_qty) ||
        (!terminal && o.leaves_qty == 0) ||
        (o.state == model::OrderState::kFilled &&
         o.cumulative_filled_qty != o.original_qty) ||
        ((o.state == model::OrderState::kRejected ||
          o.state == model::OrderState::kPendingNew) &&
         o.cumulative_filled_qty != 0) ||
        (o.cancel_pending && (o.cancel_request_id.value == 0 ||
                              (o.state != model::OrderState::kWorking &&
                               o.state != model::OrderState::kFilled))) ||
        (record.cancel_attempted && !o.cancel_pending) ||
        (record.local_cancel_pending &&
         (record.new_attempted || o.state != model::OrderState::kPendingNew)) ||
        (!record.new_attempted && o.report_sequence != 0) ||
        (o.report_sequence != 0 && o.broker_order_id.empty() &&
         o.state != model::OrderState::kRejected) ||
        (o.state == model::OrderState::kWorking && o.report_sequence == 0))
      return false;
    const auto expected_cash = !terminal && o.side == model::OrderSide::kBuy
                                   ? o.limit_price * o.leaves_qty * scale
                                   : 0;
    const auto expected_qty =
        !terminal && o.side == model::OrderSide::kSell ? o.leaves_qty : 0;
    if (record.reserved_cash != expected_cash ||
        record.reserved_qty != expected_qty ||
        record.reserved_cash > kMaximum - reserved_cash ||
        record.reserved_qty > kMaximum - quantities[record.position_index])
      return false;
    reserved_cash += record.reserved_cash;
    quantities[record.position_index] += record.reserved_qty;
    active += !terminal;
    pending_cancel += o.cancel_pending;
    if (!o.broker_order_id.empty()) {
      auto slot = TradingBrokerHash(o.broker_order_id);
      while (s.broker_lookup[slot] != 0) {
        if (s.orders[s.broker_lookup[slot] - 1].snapshot.broker_order_id ==
            o.broker_order_id)
          return false;
        slot = (slot + 1) & (kBrokerLookupCapacity - 1);
      }
      s.broker_lookup[slot] = static_cast<std::uint16_t>(i + 1);
    }
  }
  if (active != s.active_count || pending_cancel != s.pending_cancel_count ||
      reserved_cash != s.cash.reserved)
    return false;
  for (std::size_t i = 0; i < s.position_count; ++i)
    if (quantities[i] != s.positions[i].reserved_qty) return false;
  for (std::size_t i = 0; i < s.fill_count; ++i) {
    const auto& f = s.fills[i];
    std::size_t index = 0;
    while (index < s.order_count &&
           s.orders[index].snapshot.client_order_id != f.client_order_id)
      ++index;
    if (index == s.order_count) return false;
    const auto& o = s.orders[index].snapshot;
    model::InstrumentDefinition instrument;
    if (!market.TryGetInstrument(f.instrument_id, instrument) ||
        instrument.price_tick <= 0 || instrument.quantity_step <= 0 ||
        f.position_effect != model::PositionEffect::kNone ||
        f.price % instrument.price_tick != 0 ||
        f.quantity % instrument.quantity_step != 0 ||
        f.broker_order_id.empty() || f.execution_id.empty() ||
        f.broker_order_id != o.broker_order_id ||
        f.instrument_id != o.instrument_id || f.side != o.side ||
        f.price <= 0 || f.quantity <= 0 ||
        (!f.has_event_time && f.event_time_ns != 0) ||
        (o.side == model::OrderSide::kBuy && f.price > o.limit_price) ||
        (o.side == model::OrderSide::kSell && f.price < o.limit_price) ||
        f.quantity > o.cumulative_filled_qty - filled[index])
      return false;
    filled[index] += f.quantity;
    auto slot = TradingFillHash(f.client_order_id, f.execution_id);
    while (s.fill_lookup[slot] != 0) {
      const auto& prior = s.fills[s.fill_lookup[slot] - 1];
      if (prior.client_order_id == f.client_order_id &&
          prior.execution_id == f.execution_id)
        return false;
      slot = (slot + 1) & (kFillLookupCapacity - 1);
    }
    s.fill_lookup[slot] = static_cast<std::uint16_t>(i + 1);
  }
  for (std::size_t i = 0; i < s.order_count; ++i)
    if (filled[i] != s.orders[i].snapshot.cumulative_filled_qty) return false;
  for (std::size_t i = 0; i < s.request_count; ++i) {
    const auto& request = s.requests[i];
    if (request.order_index >= s.order_count) return false;
    const auto index = request.order_index;
    const auto& record = s.orders[index];
    if (request.kind == Storage::RequestKind::kNew) {
      if (queued_new[index] || record.new_attempted ||
          record.local_cancel_pending ||
          record.snapshot.state != model::OrderState::kPendingNew ||
          request.cancel_id.value != 0)
        return false;
      queued_new[index] = true;
    } else if (request.kind == Storage::RequestKind::kCancel) {
      if (queued_cancel[index] || record.cancel_attempted ||
          !record.snapshot.cancel_pending ||
          request.cancel_id != record.snapshot.cancel_request_id)
        return false;
      queued_cancel[index] = true;
    } else
      return false;
  }
  for (std::size_t i = 0; i < s.local_count; ++i) {
    const auto index = s.local_updates[i];
    if (index >= s.order_count || local[index] ||
        !s.orders[index].local_cancel_pending)
      return false;
    local[index] = true;
  }
  for (std::size_t i = 0; i < s.order_count; ++i) {
    const auto& record = s.orders[i];
    if (record.local_cancel_pending != local[i] ||
        (!record.new_attempted &&
         record.snapshot.state == model::OrderState::kPendingNew && !local[i] &&
         !queued_new[i]) ||
        (record.snapshot.cancel_pending && !record.cancel_attempted &&
         !queued_cancel[i]))
      return false;
  }
  return true;
}
}  // namespace

base::Status EncodeTradingMutation(const TradingMutation& value,
                                   std::span<std::byte> bytes,
                                   std::size_t& size) noexcept {
  // Version 1 represents cash equities, whose position effect is implicit.
  // Reject unsupported active fields before writing instead of dropping them.
  if (value.revision == 0 || value.order_id.value == 0 ||
      (value.kind == TradingMutationKind::kNewOrder &&
       (value.request.position_effect != model::PositionEffect::kNone ||
        IsFuturesInstrument(value.request.instrument_id))) ||
      (value.kind == TradingMutationKind::kReport &&
       (value.event.fill.position_effect != model::PositionEffect::kNone ||
        IsFuturesInstrument(value.event.fill.instrument_id))))
    return {ErrorCode::kInvalidArgument, {}};
  Writer w(bytes);
  w.Uint(kMutationVersion, 2);
  w.Uint(static_cast<std::uint8_t>(value.kind), 1);
  w.Uint(value.revision);
  w.Uint(value.now);
  w.Uint(value.order_id.value);
  w.Uint(value.cancel_id.value);
  switch (value.kind) {
    case TradingMutationKind::kNewOrder:
      WriteRequest(w, value.request);
      break;
    case TradingMutationKind::kReport:
      WriteEvent(w, value.event);
      break;
    case TradingMutationKind::kCancel:
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel:
    case TradingMutationKind::kLocalCancel:
      break;
    default:
      return {ErrorCode::kInvalidArgument, {}};
  }
  if (!w.ok()) return {ErrorCode::kResourceExhausted, {}};
  size = w.size();
  return {};
}

base::Status DecodeTradingMutation(std::span<const std::byte> bytes,
                                   TradingMutation& out) noexcept {
  Reader r(bytes);
  if (r.Uint(2) != kMutationVersion) return {ErrorCode::kCorrupt, {}};
  TradingMutation value;
  value.kind = static_cast<TradingMutationKind>(r.Uint(1));
  value.revision = r.Uint();
  value.now = r.Uint();
  value.order_id.value = r.Uint();
  value.cancel_id.value = r.Uint();
  switch (value.kind) {
    case TradingMutationKind::kNewOrder:
      ReadRequest(r, value.request);
      break;
    case TradingMutationKind::kReport:
      ReadEvent(r, value.event);
      break;
    case TradingMutationKind::kCancel:
    case TradingMutationKind::kSendNew:
    case TradingMutationKind::kSendCancel:
    case TradingMutationKind::kScheduleLocalCancel:
    case TradingMutationKind::kLocalCancel:
      break;
    default:
      return {ErrorCode::kCorrupt, {}};
  }
  if (!r.done() || value.revision == 0 || value.order_id.value == 0 ||
      (value.kind == TradingMutationKind::kNewOrder &&
       IsFuturesInstrument(value.request.instrument_id)) ||
      (value.kind == TradingMutationKind::kReport &&
       IsFuturesInstrument(value.event.fill.instrument_id)))
    return {ErrorCode::kCorrupt, {}};
  out = value;
  return {};
}

base::Status TradingEngine::ExportCheckpoint(std::span<std::byte> bytes,
                                             std::size_t& size) const noexcept {
  if (storage_ == nullptr || accept_requests_)
    return {ErrorCode::kInvalidState, {}};
  const auto& s = *storage_;
  if (s.type != model::AccountType::kCash) return {ErrorCode::kUnsupported, {}};
  for (std::size_t i = 0; i < s.order_count; ++i)
    if (s.orders[i].snapshot.position_effect != model::PositionEffect::kNone)
      return {ErrorCode::kUnsupported, {}};
  for (std::size_t i = 0; i < s.fill_count; ++i)
    if (s.fills[i].position_effect != model::PositionEffect::kNone)
      return {ErrorCode::kUnsupported, {}};
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
  for (std::size_t i = 0; i < s.position_count; ++i) {
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
  for (std::size_t i = 0; i < s.order_count; ++i) {
    const auto& record = s.orders[i];
    WriteOrder(w, record.snapshot);
    w.Uint(record.new_attempted, 1);
    w.Uint(record.cancel_attempted, 1);
    w.Uint(record.local_cancel_pending, 1);
    w.Uint(record.retry_not_before_ns);
    w.Uint(record.position_index, 4);
    w.Signed(record.price_scale);
    w.Signed(record.reserved_cash);
    w.Signed(record.reserved_qty);
  }
  for (std::size_t i = 0; i < s.fill_count; ++i) WriteFill(w, s.fills[i]);
  w.Uint(s.request_count, 4);
  for (std::size_t i = 0; i < s.request_count; ++i) {
    const auto& request =
        s.requests[(s.request_head + i) % kMaxTradingRequests];
    w.Uint(static_cast<std::uint8_t>(request.kind), 1);
    w.Uint(request.order_index, 4);
    w.Uint(request.cancel_id.value);
  }
  w.Uint(s.local_count, 4);
  for (std::size_t i = 0; i < s.local_count; ++i)
    w.Uint(s.local_updates[(s.local_head + i) % kMaxLocalOrderUpdates], 4);
  if (!w.ok()) return {ErrorCode::kResourceExhausted, {}};
  size = w.size();
  return {};
}

base::Status TradingEngine::RestoreCheckpoint(
    std::span<const std::byte> bytes) noexcept {
  if (accept_requests_ || replaying_)
    return {
        Fail(ErrorCode::kInvalidState, "cannot restore an executing ledger"),
        {}};
  if (storage_ != nullptr && storage_->type != model::AccountType::kCash)
    return {Fail(ErrorCode::kUnsupported, "checkpoint restore requires cash"),
            {}};
  if (bytes.size() > kMaxTradingCheckpointBytes)
    return {Fail(ErrorCode::kCorrupt, "oversized ledger checkpoint"), {}};
  auto candidate = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!candidate) return {ErrorCode::kResourceExhausted, {}};
  Reader r(bytes);
  if (r.Uint() != kCheckpointMagic || r.Uint(4) != kCheckpointVersion)
    return {Fail(ErrorCode::kCorrupt, "unsupported ledger checkpoint"), {}};
  auto& s = *candidate;
  s.mutation_revision = r.Uint();
  s.next_order_id = r.Uint();
  s.last_cancel_id = r.Uint();
  s.cash.balance = r.Signed();
  s.cash.reserved = r.Signed();
  s.cash.available = r.Signed();
  s.position_count = r.Uint(4);
  if (s.position_count > md::kMaxMarketDataInstruments)
    return {ErrorCode::kCorrupt, {}};
  for (std::size_t i = 0; i < s.position_count; ++i) {
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
  if (has_retry)
    s.next_retry = retry;
  else if (retry != 0)
    r.Reject();
  if (s.order_count > kMaxTradingOrders || s.fill_count > kMaxTradingFills ||
      s.active_count > s.order_count || s.pending_cancel_count > s.order_count)
    return {ErrorCode::kCorrupt, {}};
  for (std::size_t i = 0; i < s.order_count; ++i) {
    auto& record = s.orders[i];
    ReadOrder(r, record.snapshot);
    record.new_attempted = r.Boolean();
    record.cancel_attempted = r.Boolean();
    record.local_cancel_pending = r.Boolean();
    record.retry_not_before_ns = r.Uint();
    record.position_index = r.Uint(4);
    record.price_scale = r.Signed();
    record.reserved_cash = r.Signed();
    record.reserved_qty = r.Signed();
  }
  for (std::size_t i = 0; i < s.fill_count; ++i) ReadFill(r, s.fills[i]);
  s.request_count = r.Uint(4);
  if (s.request_count > kMaxTradingRequests) return {ErrorCode::kCorrupt, {}};
  for (std::size_t i = 0; i < s.request_count; ++i) {
    auto& request = s.requests[i];
    request.kind = static_cast<Storage::RequestKind>(r.Uint(1));
    request.order_index = r.Uint(4);
    request.cancel_id.value = r.Uint();
  }
  s.local_count = r.Uint(4);
  if (s.local_count > kMaxLocalOrderUpdates) return {ErrorCode::kCorrupt, {}};
  for (std::size_t i = 0; i < s.local_count; ++i)
    s.local_updates[i] = r.Uint(4);
  if (!r.done() || !Validate(s, instruments_))
    return {Fail(ErrorCode::kCorrupt, "invalid ledger checkpoint invariants"),
            {}};
  delete storage_;
  storage_ = candidate.release();
  stopping_ = false;
  error_detail_[0] = '\0';
  return {};
}
base::Status TradingEngine::RestoreReconciled(
    const TradingAccountSnapshot& baseline,
    std::span<const model::OrderSnapshot> terminal_orders,
    std::span<const model::Fill> executions) noexcept {
  if (accept_requests_ || replaying_)
    return {
        Fail(ErrorCode::kInvalidState, "cannot reconcile an executing ledger"),
        {}};
  if (baseline.type != model::AccountType::kCash ||
      baseline.futures_position_count != 0 ||
      (storage_ != nullptr && storage_->type != model::AccountType::kCash))
    return {Fail(ErrorCode::kUnsupported, "reconciliation requires cash"), {}};
  if (baseline.position_count > md::kMaxMarketDataInstruments ||
      terminal_orders.size() > kMaxTradingOrders ||
      executions.size() > kMaxTradingFills)
    return {Fail(ErrorCode::kResourceExhausted,
                 "reconciled state exceeds capacity"),
            {}};
  auto candidate = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!candidate) return {ErrorCode::kResourceExhausted, {}};
  auto& s = *candidate;
  s.cash = {baseline.cash_balance, 0, baseline.cash_balance};
  s.next_order_id = next_order_id();
  if (storage_ != nullptr) s.last_cancel_id = storage_->last_cancel_id;
  s.position_count = baseline.position_count;
  for (std::size_t i = 0; i < s.position_count; ++i) {
    if (baseline.positions[i].reserved_qty != 0)
      return {
          Fail(ErrorCode::kCorrupt, "reconciled baseline retains reservations"),
          {}};
    s.positions[i] = baseline.positions[i];
  }
  s.order_count = terminal_orders.size();
  for (std::size_t i = 0; i < s.order_count; ++i) {
    const auto& order = terminal_orders[i];
    if (order.position_effect != model::PositionEffect::kNone ||
        !model::IsTerminalOrderState(order.state) || order.cancel_pending ||
        order.client_order_id.value == 0 ||
        order.client_order_id.value == UINT64_MAX)
      return {Fail(ErrorCode::kCorrupt, "reconciled order is not a final fact"),
              {}};
    auto& record = s.orders[i];
    record.snapshot = order;
    record.new_attempted = true;
    model::InstrumentDefinition instrument;
    if (!instruments_.TryGetInstrument(order.instrument_id, instrument) ||
        instrument.type != model::InstrumentType::kEquity ||
        instrument.price_decimals > model::kMaxPriceDecimals)
      return {Fail(ErrorCode::kCorrupt,
                   "unknown reconciled instrument or precision"),
              {}};
    record.price_scale = 1;
    for (std::size_t digit = instrument.price_decimals; digit < 6; ++digit)
      record.price_scale *= 10;
    auto position = std::size_t{0};
    while (position < s.position_count &&
           s.positions[position].instrument_id != order.instrument_id)
      ++position;
    if (position == s.position_count) {
      if (s.position_count == md::kMaxMarketDataInstruments)
        return {ErrorCode::kResourceExhausted, {}};
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
              return left.snapshot.client_order_id.value <
                     right.snapshot.client_order_id.value;
            });
  s.fill_count = executions.size();
  for (std::size_t i = 0; i < s.fill_count; ++i) s.fills[i] = executions[i];
  if (!Validate(s, instruments_))
    return {Fail(ErrorCode::kCorrupt,
                 "invalid reconciled account or execution history"),
            {}};
  delete storage_;
  storage_ = candidate.release();
  stopping_ = false;
  error_detail_[0] = '\0';
  return {};
}
}  // namespace yquant::trading
