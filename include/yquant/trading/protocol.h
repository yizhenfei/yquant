#ifndef YQUANT_TRADING_PROTOCOL_H_
#define YQUANT_TRADING_PROTOCOL_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <span>

#include "yquant/base/status.h"
#include "yquant/model/account.h"
#include "yquant/model/futures.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {

inline constexpr size_t kMaxClients = 16;
// First wire version carrying distinct strategy and gateway order identities.
inline constexpr uint16_t kOrderIdentityWireVersion = 4;
// First wire version representing a cancel completed before transmission.
inline constexpr uint16_t kLocalOrderResultWireVersion = 5;
inline constexpr size_t kMaxInstruments = 64;
inline constexpr size_t kMaxTokenBytes = 64;
// A full 64-instrument futures view plus an order/fill fits in one frame.
inline constexpr size_t kMaxFrameSize = 16384;
inline constexpr size_t kConnectionQueueFrames = 64;

struct Endpoint {
  // NUL-terminated numeric IPv4. DNS is not used by the wire transport.
  char address[16] = "127.0.0.1";
  uint16_t port = 19200;
};

// A complete authoritative account view. Its revision never decreases within
// a daemon generation. Omitted configured instruments have zero holdings.
struct AccountView {
  uint64_t revision = 0;
  model::CashSnapshot cash;
  model::AccountType type = model::AccountType::kCash;
  // True only for futures financially managed by an external broker. Such
  // views must not receive simulator quotes or local FIFO/fee recalculations.
  bool broker_managed = false;
  model::FuturesAccountSnapshot futures;
  std::array<model::FuturesPositionSnapshot, kMaxInstruments>
      futures_positions = {};
  size_t futures_position_count = 0;
  std::array<model::PositionSnapshot, kMaxInstruments> positions = {};
  size_t position_count = 0;
};

struct InstrumentSpec {
  model::InstrumentId id;
  uint8_t price_decimals = 0;
};

// Message meanings are direction-specific and checked by the session owner.
// The codec validates only framing and the selected payload's invariants.
enum class MessageType : uint16_t {
  kHello = 1,
  kWelcome,
  kNewOrder,
  kCancelOrder,
  kOrderUpdate,
  kAccount,
  kStop,
  kDrained,
  kHeartbeat,
  kError,
  // Only a simulated-futures gateway accepts client supplied quote input.
  kSimulationQuote
};

// Owned wire values. Only fields selected by type are serialized. Connection
// assigns a contiguous sequence starting at one in each direction. Hello uses
// generation zero; all subsequent messages carry the welcomed generation.
// Updates pair an owned order/fill with its fully committed account view.
// Hello carries client_id/token/instruments; Welcome carries instruments and
// account. NewOrder carries one order identity/request; CancelOrder carries one
// order identity, cancel_id and an optional adapter_order_id. OrderUpdate
// carries update, notify_order, has_fill, an optional fill, and account;
// Account carries account. Stop/Drained/Heartbeat are empty; Error carries
// error_code/reason. Gateway initiated cancellations use zero update
// cancel_request_id; client cancel commands require a nonzero cancel_id.
// Inactive fields are ignored.
struct Message {
  MessageType type = MessageType::kHeartbeat;
  uint64_t generation = 0;
  uint64_t sequence = 0;
  uint64_t client_id = 0;
  char token[kMaxTokenBytes + 1] = {};
  std::array<InstrumentSpec, kMaxInstruments> instruments = {};
  size_t instrument_count = 0;
  // Exactly one request identity is set: strategy on the strategy gateway
  // connection, client on the adapter bridge connection.
  model::ClientOrderId client_order_id;
  model::StrategyOrderId strategy_order_id;
  model::OrderRequest request;
  model::QuoteTick quote;
  model::CancelRequestId cancel_id;
  model::AdapterOrderId adapter_order_id;
  model::OrderUpdate update;
  model::Fill fill;
  bool notify_order = true;
  bool has_fill = false;
  AccountView account;
  base::ErrorCode error_code = base::ErrorCode::kOk;
  model::OrderReason reason;
};

// Wire version 5 adds CancelNotSent local order updates. Version 4 separates
// strategy and gateway order identities; readers retain v1-v4 support for
// older peers. Legacy identity scope is resolved by the connection owner.
// Version 3 adds broker-managed futures identity.
// Simulation quote frames reuse the market-data quote codec; live gateways
// reject them. Explicit little-endian, versioned framing. Encoding/decoding are
// bounded, allocation-free, reject noncanonical data and preserve outputs on
// failure.
[[nodiscard]] base::Status EncodeMessage(const Message& message,
                                         std::span<std::byte> output,
                                         size_t& size) noexcept;
[[nodiscard]] base::Status DecodeMessage(std::span<const std::byte> input,
                                         Message& output) noexcept;
// Returns the full frame size from an exact fixed-size header, or zero for an
// invalid header. Used only to delimit a stream before DecodeMessage.
inline constexpr size_t kFrameHeaderSize = 32;
size_t FrameSize(std::span<const std::byte> header) noexcept;

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_PROTOCOL_H_
