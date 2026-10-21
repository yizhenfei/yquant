#ifndef YQUANT_TRADINGD_PROTOCOL_H_
#define YQUANT_TRADINGD_PROTOCOL_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "yquant/base/status.h"
#include "yquant/model/account.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::tradingd {

inline constexpr std::size_t kMaxClients = 16;
inline constexpr std::size_t kMaxInstruments = 64;
inline constexpr std::size_t kMaxTokenBytes = 64;
inline constexpr std::size_t kMaxFrameSize = 4096;
inline constexpr std::size_t kConnectionQueueFrames = 64;

struct Endpoint {
  // NUL-terminated numeric IPv4. DNS is not used by the wire transport.
  char address[16] = "127.0.0.1";
  std::uint16_t port = 19200;
};

// A complete authoritative account view. Its revision never decreases within
// a daemon generation. Omitted configured instruments have zero holdings.
struct AccountView {
  std::uint64_t revision = 0;
  model::CashSnapshot cash;
  std::array<model::PositionSnapshot, kMaxInstruments> positions = {};
  std::size_t position_count = 0;
};

struct InstrumentSpec {
  model::InstrumentId id;
  std::uint8_t price_decimals = 0;
};

// Message meanings are direction-specific and checked by the session owner.
// The codec validates only framing and the selected payload's invariants.
enum class MessageType : std::uint16_t {
  kHello = 1,
  kWelcome,
  kNewOrder,
  kCancelOrder,
  kOrderUpdate,
  kAccount,
  kStop,
  kDrained,
  kHeartbeat,
  kError
};

// Owned wire values. Only fields selected by type are serialized. Connection
// assigns a contiguous sequence starting at one in each direction. Hello uses
// generation zero; all subsequent messages carry the welcomed generation.
// Updates pair an owned order/fill with its fully committed account view.
// Hello carries client_id/token/instruments; Welcome carries instruments and
// account. NewOrder carries order_id/request; CancelOrder carries order_id,
// cancel_id and an optional broker_order_id. OrderUpdate carries update,
// notify_order, has_fill, an optional fill, and account; Account carries
// account. Stop/Drained/Heartbeat are empty; Error carries error_code/reason.
// Gateway initiated cancellations use zero update cancel_request_id; client
// cancel commands require a nonzero cancel_id. Inactive fields are ignored.
struct Message {
  MessageType type = MessageType::kHeartbeat;
  std::uint64_t generation = 0;
  std::uint64_t sequence = 0;
  std::uint64_t client_id = 0;
  char token[kMaxTokenBytes + 1] = {};
  std::array<InstrumentSpec, kMaxInstruments> instruments = {};
  std::size_t instrument_count = 0;
  model::ClientOrderId order_id;
  model::OrderRequest request;
  model::CancelRequestId cancel_id;
  model::BrokerOrderId broker_order_id;
  model::OrderUpdate update;
  model::Fill fill;
  bool notify_order = true;
  bool has_fill = false;
  AccountView account;
  base::ErrorCode error_code = base::ErrorCode::kOk;
  model::OrderReason reason;
};

// Wire version 1 supports equity instruments and PositionEffect::kNone only;
// encoding a futures effect fails instead of silently dropping the field.
// Explicit little-endian, versioned framing. Encoding/decoding are bounded,
// allocation-free, reject noncanonical data and preserve outputs on failure.
[[nodiscard]] base::Status EncodeMessage(const Message& message,
                                         std::span<std::byte> output,
                                         std::size_t& size) noexcept;
[[nodiscard]] base::Status DecodeMessage(std::span<const std::byte> input,
                                         Message& output) noexcept;
// Returns the full frame size from an exact fixed-size header, or zero for an
// invalid header. Used only to delimit a stream before DecodeMessage.
inline constexpr std::size_t kFrameHeaderSize = 32;
std::size_t FrameSize(std::span<const std::byte> header) noexcept;

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_PROTOCOL_H_
