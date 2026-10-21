#include "tradingd/protocol.h"

#include <bit>
#include <cstring>
#include <limits>
#include <string_view>

namespace yquant::tradingd {
namespace {

// The byte spelling is YQTR. All integer fields use little-endian order.
constexpr std::uint32_t kMagic = 0x52545159;
constexpr std::uint16_t kVersion = 1;
constexpr std::size_t kMaxReasonBytes = sizeof(model::OrderReason{}.text) - 1;
constexpr std::uint8_t kMaxPriceDecimals = 6;
constexpr std::uint32_t kMaxSymbolValue = 999999;

bool ValidType(MessageType type) noexcept {
  return type >= MessageType::kHello && type <= MessageType::kError;
}

bool ValidInstrument(model::InstrumentId instrument) noexcept {
  const auto venue = instrument.value >> 32;
  return venue >= static_cast<std::uint64_t>(model::Venue::kSse) &&
         venue <= static_cast<std::uint64_t>(model::Venue::kBse) &&
         (instrument.value & 0xffffffffU) <= kMaxSymbolValue;
}

bool ValidSide(model::OrderSide side) noexcept {
  return side == model::OrderSide::kBuy || side == model::OrderSide::kSell;
}

// Validate the unscaled notional here; instrument-specific cash conversion is
// an account-layer responsibility after the instrument dictionary is known.
bool ValidAmount(std::int64_t price, std::int64_t quantity) noexcept {
  return price > 0 && quantity > 0 &&
         price <= std::numeric_limits<std::int64_t>::max() / quantity;
}

bool ValidOrder(const model::OrderSnapshot& order) noexcept {
  if (order.position_effect != model::PositionEffect::kNone ||
      order.client_order_id.value == 0 ||
      !ValidInstrument(order.instrument_id) || !ValidSide(order.side) ||
      !ValidAmount(order.limit_price, order.original_qty) ||
      order.cumulative_filled_qty < 0 ||
      order.cumulative_filled_qty > order.original_qty ||
      order.leaves_qty < 0 || order.state < model::OrderState::kPendingNew ||
      order.state > model::OrderState::kRejected) {
    return false;
  }
  if (model::IsTerminalOrderState(order.state)) {
    return order.leaves_qty == 0 &&
           (order.state != model::OrderState::kFilled ||
            order.cumulative_filled_qty == order.original_qty);
  }
  return order.leaves_qty == order.original_qty - order.cumulative_filled_qty;
}

bool ValidFill(const model::Fill& fill,
               const model::OrderSnapshot& order) noexcept {
  return fill.position_effect == model::PositionEffect::kNone &&
         fill.client_order_id == order.client_order_id &&
         fill.broker_order_id == order.broker_order_id &&
         !fill.broker_order_id.empty() && !fill.execution_id.empty() &&
         fill.instrument_id == order.instrument_id && fill.side == order.side &&
         ValidAmount(fill.price, fill.quantity) &&
         fill.quantity <= order.cumulative_filled_qty &&
         (fill.has_event_time || fill.event_time_ns == 0);
}

bool ValidAccount(const AccountView& account) noexcept {
  if (account.position_count > kMaxInstruments || account.cash.balance < 0 ||
      account.cash.reserved < 0 ||
      account.cash.reserved > account.cash.balance ||
      account.cash.available != account.cash.balance - account.cash.reserved) {
    return false;
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    if (!ValidInstrument(position.instrument_id) || position.total_qty < 0 ||
        position.available_qty < 0 || position.reserved_qty < 0 ||
        position.available_qty > position.total_qty ||
        position.reserved_qty > position.total_qty - position.available_qty) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (account.positions[previous].instrument_id == position.instrument_id)
        return false;
    }
  }
  return true;
}

bool ValidInstruments(const Message& message) noexcept {
  if (message.instrument_count == 0 ||
      message.instrument_count > kMaxInstruments)
    return false;
  for (std::size_t index = 0; index < message.instrument_count; ++index) {
    const auto& instrument = message.instruments[index];
    if (!ValidInstrument(instrument.id) ||
        instrument.price_decimals > kMaxPriceDecimals)
      return false;
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (message.instruments[previous].id == instrument.id) return false;
    }
  }
  return true;
}

// Cursor helpers have a sticky bounds failure and never access partial fields.
// Each cursor borrows storage for one EncodeMessage/DecodeMessage call only.
class Writer {
 public:
  explicit Writer(std::span<std::byte> bytes) noexcept : bytes_(bytes) {}
  void Uint(std::uint64_t value, std::size_t width) noexcept {
    if (!Reserve(width)) return;
    for (std::size_t index = 0; index < width; ++index) {
      bytes_[size_++] = static_cast<std::byte>(value & 0xffU);
      value >>= 8;
    }
  }
  void Signed(std::int64_t value) noexcept {
    Uint(std::bit_cast<std::uint64_t>(value), sizeof(value));
  }
  void Bytes(std::string_view value) noexcept {
    if (!Reserve(value.size())) return;
    if (!value.empty())
      std::memcpy(bytes_.data() + size_, value.data(), value.size());
    size_ += value.size();
  }
  std::size_t size() const noexcept { return size_; }
  bool ok() const noexcept { return ok_; }

 private:
  bool Reserve(std::size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) {
      ok_ = false;
      return false;
    }
    return true;
  }
  std::span<std::byte> bytes_;
  std::size_t size_ = 0;
  bool ok_ = true;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}
  std::uint64_t Uint(std::size_t width) noexcept {
    if (!Reserve(width)) return 0;
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
      value |= static_cast<std::uint64_t>(bytes_[size_++]) << (index * 8);
    }
    return value;
  }
  std::int64_t Signed() noexcept {
    return std::bit_cast<std::int64_t>(Uint(sizeof(std::int64_t)));
  }
  std::string_view Bytes(std::size_t size) noexcept {
    if (!Reserve(size)) return {};
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + size_);
    size_ += size;
    return {begin, size};
  }
  bool Boolean() noexcept {
    const auto value = Uint(1);
    if (value > 1) ok_ = false;
    return value == 1;
  }
  bool done() const noexcept { return ok_ && size_ == bytes_.size(); }
  bool ok() const noexcept { return ok_; }
  void Reject() noexcept { ok_ = false; }

 private:
  bool Reserve(std::size_t size) noexcept {
    if (!ok_ || size > bytes_.size() - size_) {
      ok_ = false;
      return false;
    }
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
  const auto value = reader.Bytes(size);
  if (reader.ok() && size != 0 && id.Assign(value) != base::ErrorCode::kOk)
    reader.Reject();
}

bool WriteReason(Writer& writer, const model::OrderReason& reason) noexcept {
  if (reason.size > kMaxReasonBytes || reason.text[reason.size] != '\0')
    return false;
  writer.Uint(std::bit_cast<std::uint32_t>(reason.code), 4);
  writer.Uint(reason.truncated, 1);
  writer.Uint(reason.size, 1);
  writer.Bytes(reason.view());
  return writer.ok();
}

void ReadReason(Reader& reader, model::OrderReason& reason) noexcept {
  reason.code =
      std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Uint(4)));
  reason.truncated = reader.Boolean();
  const auto size = reader.Uint(1);
  if (size > kMaxReasonBytes) {
    reader.Reject();
    return;
  }
  const auto value = reader.Bytes(size);
  if (!reader.ok()) return;
  if (size != 0) std::memcpy(reason.text, value.data(), size);
  reason.text[size] = '\0';
  reason.size = static_cast<std::uint8_t>(size);
}

void WriteAccount(Writer& writer, const AccountView& account) noexcept {
  writer.Uint(account.revision, 8);
  writer.Signed(account.cash.balance);
  writer.Signed(account.cash.reserved);
  writer.Signed(account.cash.available);
  writer.Uint(account.position_count, 2);
  for (std::size_t index = 0; index < account.position_count; ++index) {
    const auto& position = account.positions[index];
    writer.Uint(position.instrument_id.value, 8);
    writer.Signed(position.total_qty);
    writer.Signed(position.available_qty);
    writer.Signed(position.reserved_qty);
  }
}

void ReadAccount(Reader& reader, AccountView& account) noexcept {
  account.revision = reader.Uint(8);
  account.cash.balance = reader.Signed();
  account.cash.reserved = reader.Signed();
  account.cash.available = reader.Signed();
  account.position_count = reader.Uint(2);
  if (account.position_count > kMaxInstruments) {
    reader.Reject();
    return;
  }
  for (std::size_t index = 0; index < account.position_count; ++index) {
    auto& position = account.positions[index];
    position.instrument_id.value = reader.Uint(8);
    position.total_qty = reader.Signed();
    position.available_qty = reader.Signed();
    position.reserved_qty = reader.Signed();
  }
  if (!ValidAccount(account)) reader.Reject();
}

void WriteInstruments(Writer& writer, const Message& message) noexcept {
  writer.Uint(message.instrument_count, 2);
  for (std::size_t index = 0; index < message.instrument_count; ++index) {
    writer.Uint(message.instruments[index].id.value, 8);
    writer.Uint(message.instruments[index].price_decimals, 1);
  }
}

void ReadInstruments(Reader& reader, Message& message) noexcept {
  message.instrument_count = reader.Uint(2);
  if (message.instrument_count > kMaxInstruments) {
    reader.Reject();
    return;
  }
  for (std::size_t index = 0; index < message.instrument_count; ++index) {
    message.instruments[index].id.value = reader.Uint(8);
    message.instruments[index].price_decimals =
        static_cast<std::uint8_t>(reader.Uint(1));
  }
  if (!ValidInstruments(message)) reader.Reject();
}

void WriteOrder(Writer& writer, const model::OrderSnapshot& order) noexcept {
  writer.Uint(order.client_order_id.value, 8);
  WriteId(writer, order.broker_order_id);
  writer.Uint(order.instrument_id.value, 8);
  writer.Uint(static_cast<std::uint8_t>(order.side), 1);
  writer.Signed(order.limit_price);
  writer.Signed(order.original_qty);
  writer.Signed(order.cumulative_filled_qty);
  writer.Signed(order.leaves_qty);
  writer.Uint(static_cast<std::uint8_t>(order.state), 1);
  writer.Uint(order.cancel_pending, 1);
  writer.Uint(order.cancel_request_id.value, 8);
  writer.Uint(order.report_sequence, 8);
  writer.Uint(order.receive_time_ns, 8);
}

void ReadOrder(Reader& reader, model::OrderSnapshot& order) noexcept {
  order.client_order_id.value = reader.Uint(8);
  ReadId(reader, order.broker_order_id);
  order.instrument_id.value = reader.Uint(8);
  order.side = static_cast<model::OrderSide>(reader.Uint(1));
  order.limit_price = reader.Signed();
  order.original_qty = reader.Signed();
  order.cumulative_filled_qty = reader.Signed();
  order.leaves_qty = reader.Signed();
  order.state = static_cast<model::OrderState>(reader.Uint(1));
  order.cancel_pending = reader.Boolean();
  order.cancel_request_id.value = reader.Uint(8);
  order.report_sequence = reader.Uint(8);
  order.receive_time_ns = reader.Uint(8);
  if (!ValidOrder(order)) reader.Reject();
}

void WriteFill(Writer& writer, const model::Fill& fill) noexcept {
  writer.Uint(fill.client_order_id.value, 8);
  WriteId(writer, fill.broker_order_id);
  WriteId(writer, fill.execution_id);
  writer.Uint(fill.instrument_id.value, 8);
  writer.Uint(static_cast<std::uint8_t>(fill.side), 1);
  writer.Signed(fill.price);
  writer.Signed(fill.quantity);
  writer.Uint(fill.event_time_ns, 8);
  writer.Uint(fill.receive_time_ns, 8);
  writer.Uint(fill.has_event_time, 1);
}

void ReadFill(Reader& reader, model::Fill& fill) noexcept {
  fill.client_order_id.value = reader.Uint(8);
  ReadId(reader, fill.broker_order_id);
  ReadId(reader, fill.execution_id);
  fill.instrument_id.value = reader.Uint(8);
  fill.side = static_cast<model::OrderSide>(reader.Uint(1));
  fill.price = reader.Signed();
  fill.quantity = reader.Signed();
  fill.event_time_ns = reader.Uint(8);
  fill.receive_time_ns = reader.Uint(8);
  fill.has_event_time = reader.Boolean();
}

bool ValidUpdate(const Message& message) noexcept {
  return ValidOrder(message.update.order) &&
         message.update.kind >= model::OrderUpdateKind::kAccepted &&
         message.update.kind <= model::OrderUpdateKind::kCanceledBeforeSend &&
         (!message.has_fill ||
          (message.update.kind == model::OrderUpdateKind::kFill &&
           ValidFill(message.fill, message.update.order)));
}

bool WritePayload(Writer& writer, const Message& message) noexcept {
  switch (message.type) {
    case MessageType::kHello: {
      const auto* end = static_cast<const char*>(
          std::memchr(message.token, '\0', sizeof(message.token)));
      if (message.client_id == 0 || end == nullptr || end == message.token ||
          !ValidInstruments(message))
        return false;
      writer.Uint(message.client_id, 8);
      const std::string_view token(message.token, end - message.token);
      writer.Uint(token.size(), 1);
      writer.Bytes(token);
      WriteInstruments(writer, message);
      break;
    }
    case MessageType::kWelcome:
      if (!ValidInstruments(message) || !ValidAccount(message.account))
        return false;
      WriteInstruments(writer, message);
      WriteAccount(writer, message.account);
      break;
    case MessageType::kNewOrder:
      if (message.order_id.value == 0 ||
          message.request.position_effect != model::PositionEffect::kNone ||
          !ValidInstrument(message.request.instrument_id) ||
          !ValidSide(message.request.side) ||
          !ValidAmount(message.request.limit_price, message.request.quantity))
        return false;
      writer.Uint(message.order_id.value, 8);
      writer.Uint(message.request.instrument_id.value, 8);
      writer.Uint(static_cast<std::uint8_t>(message.request.side), 1);
      writer.Signed(message.request.limit_price);
      writer.Signed(message.request.quantity);
      break;
    case MessageType::kCancelOrder:
      if (message.order_id.value == 0 || message.cancel_id.value == 0)
        return false;
      writer.Uint(message.order_id.value, 8);
      writer.Uint(message.cancel_id.value, 8);
      WriteId(writer, message.broker_order_id);
      break;
    case MessageType::kOrderUpdate:
      if (!ValidUpdate(message) || !ValidAccount(message.account)) return false;
      writer.Uint(message.notify_order, 1);
      writer.Uint(message.has_fill, 1);
      WriteOrder(writer, message.update.order);
      writer.Uint(static_cast<std::uint8_t>(message.update.kind), 1);
      writer.Uint(message.update.cancel_request_id.value, 8);
      if (!WriteReason(writer, message.update.reason)) return false;
      if (message.has_fill) WriteFill(writer, message.fill);
      WriteAccount(writer, message.account);
      break;
    case MessageType::kAccount:
      if (!ValidAccount(message.account)) return false;
      WriteAccount(writer, message.account);
      break;
    case MessageType::kError:
      if (message.error_code <= base::ErrorCode::kOk ||
          message.error_code > base::ErrorCode::kInternalError)
        return false;
      writer.Uint(static_cast<std::uint8_t>(message.error_code), 1);
      if (!WriteReason(writer, message.reason)) return false;
      break;
    case MessageType::kStop:
    case MessageType::kDrained:
    case MessageType::kHeartbeat:
      break;
  }
  return writer.ok();
}

void ReadPayload(Reader& reader, Message& message) noexcept {
  switch (message.type) {
    case MessageType::kHello: {
      message.client_id = reader.Uint(8);
      const auto size = reader.Uint(1);
      if (size == 0 || size > kMaxTokenBytes) {
        reader.Reject();
        break;
      }
      const auto token = reader.Bytes(size);
      if (!reader.ok() || std::memchr(token.data(), '\0', size) != nullptr) {
        reader.Reject();
        break;
      }
      std::memcpy(message.token, token.data(), size);
      message.token[size] = '\0';
      ReadInstruments(reader, message);
      if (message.client_id == 0) reader.Reject();
      break;
    }
    case MessageType::kWelcome:
      ReadInstruments(reader, message);
      ReadAccount(reader, message.account);
      break;
    case MessageType::kNewOrder:
      message.order_id.value = reader.Uint(8);
      message.request.instrument_id.value = reader.Uint(8);
      message.request.side = static_cast<model::OrderSide>(reader.Uint(1));
      message.request.limit_price = reader.Signed();
      message.request.quantity = reader.Signed();
      if (message.order_id.value == 0 ||
          message.request.position_effect != model::PositionEffect::kNone ||
          !ValidInstrument(message.request.instrument_id) ||
          !ValidSide(message.request.side) ||
          !ValidAmount(message.request.limit_price, message.request.quantity))
        reader.Reject();
      break;
    case MessageType::kCancelOrder:
      message.order_id.value = reader.Uint(8);
      message.cancel_id.value = reader.Uint(8);
      ReadId(reader, message.broker_order_id);
      if (message.order_id.value == 0 || message.cancel_id.value == 0)
        reader.Reject();
      break;
    case MessageType::kOrderUpdate:
      message.notify_order = reader.Boolean();
      message.has_fill = reader.Boolean();
      ReadOrder(reader, message.update.order);
      message.update.kind = static_cast<model::OrderUpdateKind>(reader.Uint(1));
      message.update.cancel_request_id.value = reader.Uint(8);
      ReadReason(reader, message.update.reason);
      if (message.has_fill) ReadFill(reader, message.fill);
      ReadAccount(reader, message.account);
      if (!ValidUpdate(message)) reader.Reject();
      break;
    case MessageType::kAccount:
      ReadAccount(reader, message.account);
      break;
    case MessageType::kError:
      message.error_code = static_cast<base::ErrorCode>(reader.Uint(1));
      ReadReason(reader, message.reason);
      if (message.error_code <= base::ErrorCode::kOk ||
          message.error_code > base::ErrorCode::kInternalError)
        reader.Reject();
      break;
    case MessageType::kStop:
    case MessageType::kDrained:
    case MessageType::kHeartbeat:
      break;
  }
}

}  // namespace

base::Status EncodeMessage(const Message& message, std::span<std::byte> output,
                           std::size_t& size) noexcept {
  if (!ValidType(message.type) || message.sequence == 0)
    return {base::ErrorCode::kInvalidArgument, {}};
  std::array<std::byte, kMaxFrameSize> frame;
  Writer payload(std::span(frame).subspan(kFrameHeaderSize));
  if (!WritePayload(payload, message))
    return {base::ErrorCode::kInvalidArgument, {}};
  const auto frame_size = kFrameHeaderSize + payload.size();
  if (frame_size > output.size())
    return {base::ErrorCode::kResourceExhausted, {}};
  Writer header(std::span(frame).first(kFrameHeaderSize));
  header.Uint(kMagic, 4);
  header.Uint(kVersion, 2);
  header.Uint(static_cast<std::uint16_t>(message.type), 2);
  header.Uint(frame_size, 4);
  header.Uint(0, 4);
  header.Uint(message.generation, 8);
  header.Uint(message.sequence, 8);
  std::memcpy(output.data(), frame.data(), frame_size);
  size = frame_size;
  return {};
}

std::size_t FrameSize(std::span<const std::byte> header) noexcept {
  if (header.size() != kFrameHeaderSize) return 0;
  Reader reader(header);
  if (reader.Uint(4) != kMagic || reader.Uint(2) != kVersion ||
      !ValidType(static_cast<MessageType>(reader.Uint(2))))
    return 0;
  const auto size = reader.Uint(4);
  if (size < kFrameHeaderSize || size > kMaxFrameSize || reader.Uint(4) != 0)
    return 0;
  (void)reader.Uint(8);  // Generation is checked by the application handshake.
  if (reader.Uint(8) == 0 || !reader.done()) return 0;
  return size;
}

base::Status DecodeMessage(std::span<const std::byte> input,
                           Message& output) noexcept {
  if (input.size() < kFrameHeaderSize ||
      FrameSize(input.first(kFrameHeaderSize)) != input.size())
    return {base::ErrorCode::kCorrupt, {}};
  Message message;
  Reader header(input.first(kFrameHeaderSize));
  (void)header.Uint(4);
  (void)header.Uint(2);
  message.type = static_cast<MessageType>(header.Uint(2));
  (void)header.Uint(4);
  (void)header.Uint(4);
  message.generation = header.Uint(8);
  message.sequence = header.Uint(8);
  Reader payload(input.subspan(kFrameHeaderSize));
  ReadPayload(payload, message);
  if (!payload.done()) return {base::ErrorCode::kCorrupt, {}};
  output = message;
  return {};
}

}  // namespace yquant::tradingd
