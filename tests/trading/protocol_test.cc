#include "yquant/trading/protocol.h"

#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <string>

#include "gtest/gtest.h"

namespace yquant::trading {
namespace {

model::InstrumentId Instrument(std::uint32_t symbol = 600000) {
  return {(static_cast<std::uint64_t>(model::Venue::kSse) << 32) | symbol};
}

trading::Message Example(trading::MessageType type) {
  trading::Message message;
  message.type = type;
  message.sequence = 1;
  message.generation = type == trading::MessageType::kHello ? 0 : 27;
  message.client_id = 3;
  std::memcpy(message.token, "secret", 7);
  message.instrument_count = 1;
  message.instruments[0] = {Instrument(), 4};
  message.order_id = {17};
  message.request = {Instrument(), model::OrderSide::kBuy, 120, 100};
  message.cancel_id = {9};
  EXPECT_EQ(message.broker_order_id.Assign({"B\0O", 3}), base::ErrorCode::kOk);
  auto& order = message.update.order;
  order.client_order_id = message.order_id;
  order.broker_order_id = message.broker_order_id;
  order.instrument_id = Instrument();
  order.side = model::OrderSide::kBuy;
  order.limit_price = 120;
  order.original_qty = 100;
  order.cumulative_filled_qty = 10;
  order.leaves_qty = 90;
  order.state = model::OrderState::kWorking;
  order.cancel_pending = true;
  order.cancel_request_id = message.cancel_id;
  order.report_sequence = 4;
  order.receive_time_ns = 1234;
  message.update.kind = model::OrderUpdateKind::kFill;
  message.update.cancel_request_id = message.cancel_id;
  message.update.reason.Set(-4, "diagnostic");
  message.has_fill = true;
  message.fill.client_order_id = message.order_id;
  message.fill.broker_order_id = message.broker_order_id;
  EXPECT_EQ(message.fill.execution_id.Assign("execution"),
            base::ErrorCode::kOk);
  message.fill.instrument_id = Instrument();
  message.fill.side = model::OrderSide::kBuy;
  message.fill.price = 119;
  message.fill.quantity = 10;
  message.fill.event_time_ns = 999;
  message.fill.receive_time_ns = 1234;
  message.fill.has_event_time = true;
  message.account.revision = 7;
  message.account.cash = {1000, 100, 900};
  message.account.position_count = 1;
  message.account.positions[0] = {Instrument(), 100, 80, 20};
  message.error_code = base::ErrorCode::kInvalidArgument;
  message.reason.Set(-3, "failed");
  return message;
}

// Failure probes use deliberately unrelated output data, so a partial write
// cannot be hidden by a round-trip helper's zero initialization.
void ExpectEncodingRejected(const trading::Message& message) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  bytes.fill(std::byte{0xa5});
  const auto original = bytes;
  std::size_t size = 123;
  EXPECT_FALSE(trading::EncodeMessage(message, bytes, size).ok());
  EXPECT_EQ(size, 123);
  EXPECT_EQ(bytes, original);
}

class ProtocolRoundTripTest
    : public testing::TestWithParam<trading::MessageType> {};

TEST_P(ProtocolRoundTripTest, CanonicalRoundTripPreservesOwnedValues) {
  const auto original = Example(GetParam());
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(original, bytes, size).ok());
  EXPECT_EQ(
      trading::FrameSize(std::span(bytes).first(trading::kFrameHeaderSize)),
      size);
  trading::Message decoded;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.type, original.type);
  EXPECT_EQ(decoded.generation, original.generation);
  EXPECT_EQ(decoded.sequence, original.sequence);
  std::array<std::byte, trading::kMaxFrameSize> reencoded;
  std::size_t reencoded_size = 0;
  ASSERT_TRUE(trading::EncodeMessage(decoded, reencoded, reencoded_size).ok());
  EXPECT_EQ(reencoded_size, size);
  EXPECT_EQ(std::memcmp(bytes.data(), reencoded.data(), size), 0);
  if (original.type == trading::MessageType::kOrderUpdate) {
    EXPECT_EQ(decoded.update.order, original.update.order);
    EXPECT_EQ(decoded.update.reason, original.update.reason);
    EXPECT_EQ(decoded.fill.execution_id, original.fill.execution_id);
    EXPECT_EQ(decoded.fill.quantity, 10);
  }
}

INSTANTIATE_TEST_SUITE_P(AllMessages, ProtocolRoundTripTest,
                         testing::Values(trading::MessageType::kHello,
                                         trading::MessageType::kWelcome,
                                         trading::MessageType::kNewOrder,
                                         trading::MessageType::kCancelOrder,
                                         trading::MessageType::kOrderUpdate,
                                         trading::MessageType::kAccount,
                                         trading::MessageType::kStop,
                                         trading::MessageType::kDrained,
                                         trading::MessageType::kHeartbeat,
                                         trading::MessageType::kError));

TEST(TradingProtocol, HeaderHasStableLittleEndianLayout) {
  auto message = Example(trading::MessageType::kHeartbeat);
  message.generation = 0x0807060504030201ULL;
  message.sequence = 0x1817161514131211ULL;
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  ASSERT_EQ(size, 32);
  EXPECT_EQ(std::memcmp(bytes.data(), "YQTR", 4), 0);
  EXPECT_EQ(bytes[4], std::byte{1});
  EXPECT_EQ(bytes[5], std::byte{0});
  EXPECT_EQ(bytes[8], std::byte{32});
  for (std::size_t index = 0; index < 8; ++index) {
    EXPECT_EQ(bytes[16 + index], static_cast<std::byte>(index + 1));
    EXPECT_EQ(bytes[24 + index], static_cast<std::byte>(index + 17));
  }
}

TEST(TradingProtocol, DictionaryAndAccountAcceptTheirFullIndependentBounds) {
  auto message = Example(trading::MessageType::kWelcome);
  message.instrument_count = 64;
  message.account.position_count = 64;
  for (std::size_t index = 0; index < 64; ++index) {
    message.instruments[index] = {Instrument(static_cast<std::uint32_t>(index)),
                                  static_cast<std::uint8_t>(index % 7)};
    message.account.positions[index] = {message.instruments[index].id, 100, 80,
                                        20};
  }
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message decoded;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.instrument_count, 64);
  EXPECT_EQ(decoded.account.position_count, 64);
  EXPECT_EQ(decoded.account.positions[63], message.account.positions[63]);
}

TEST(TradingProtocol, InactiveFieldsDoNotAffectAHeartbeat) {
  auto message = Example(trading::MessageType::kHeartbeat);
  message.instrument_count = std::numeric_limits<std::size_t>::max();
  message.account.position_count = std::numeric_limits<std::size_t>::max();
  message.request.quantity = -1;
  std::memset(message.token, 'x', sizeof(message.token));
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  EXPECT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  EXPECT_EQ(size, 32);
}

TEST(TradingProtocol, EncodingFailurePreservesBufferAndSize) {
  auto message = Example(trading::MessageType::kHeartbeat);
  message.sequence = 0;
  ExpectEncodingRejected(message);
  message.sequence = 1;
  message.type = static_cast<trading::MessageType>(0);
  ExpectEncodingRejected(message);
  std::array<std::byte, 31> bytes;
  bytes.fill(std::byte{0xa5});
  const auto original = bytes;
  std::size_t size = 123;
  message.type = trading::MessageType::kHeartbeat;
  EXPECT_EQ(trading::EncodeMessage(message, bytes, size).code,
            base::ErrorCode::kResourceExhausted);
  EXPECT_EQ(size, 123);
  EXPECT_EQ(bytes, original);
}

TEST(TradingProtocol, RejectsInvalidHelloAndInstrumentDictionary) {
  auto message = Example(trading::MessageType::kHello);
  message.client_id = 0;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kHello);
  message.token[0] = '\0';
  ExpectEncodingRejected(message);
  std::memset(message.token, 'x', sizeof(message.token));
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kHello);
  message.instrument_count = 0;
  ExpectEncodingRejected(message);
  message.instrument_count = 65;
  ExpectEncodingRejected(message);
  message.instrument_count = 2;
  message.instruments[1] = message.instruments[0];
  ExpectEncodingRejected(message);
  message.instrument_count = 1;
  message.instruments[0].price_decimals = 7;
  ExpectEncodingRejected(message);
  message.instruments[0] = {{0x1000f4240ULL}, 4};  // Seven-digit symbol.
  ExpectEncodingRejected(message);
  message.instruments[0] = {{0x400000001ULL}, 4};  // Unknown venue.
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, RejectsInvalidOrdersAndUnscaledNotionalOverflow) {
  auto message = Example(trading::MessageType::kNewOrder);
  message.order_id.value = 0;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kNewOrder);
  message.request.side = static_cast<model::OrderSide>(3);
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kNewOrder);
  message.request.limit_price = 0;
  ExpectEncodingRejected(message);
  message.request.limit_price = -1;
  ExpectEncodingRejected(message);
  message.request.limit_price = std::numeric_limits<std::int64_t>::max();
  message.request.quantity = 2;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kNewOrder);
  message.request.quantity = 0;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kCancelOrder);
  message.cancel_id.value = 0;
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, RejectsAccountCashAndPositionInvariants) {
  auto message = Example(trading::MessageType::kAccount);
  message.account.cash.balance = -1;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kAccount);
  message.account.cash.reserved = 1001;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kAccount);
  message.account.cash.available = 899;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kAccount);
  message.account.position_count = 65;
  ExpectEncodingRejected(message);
  message.account.position_count = 2;
  message.account.positions[1] = message.account.positions[0];
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kAccount);
  message.account.positions[0].reserved_qty = 21;
  ExpectEncodingRejected(message);
  message.account.positions[0].reserved_qty = -1;
  ExpectEncodingRejected(message);
  message.account.positions[0].reserved_qty = 0;
  message.account.positions[0].available_qty = 101;
  ExpectEncodingRejected(message);
  message.account.positions[0].instrument_id = {};
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, RejectsInconsistentUpdateAndFillShapes) {
  auto message = Example(trading::MessageType::kOrderUpdate);
  message.update.order.leaves_qty = 91;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.update.order.state = model::OrderState::kFilled;
  message.update.order.leaves_qty = 0;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.fill.quantity = 11;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.fill.execution_id = {};
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.fill.client_order_id.value = 18;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.fill.has_event_time = false;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.update.kind = model::OrderUpdateKind::kAccepted;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.update.reason.size = 129;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kOrderUpdate);
  message.update.reason.text[message.update.reason.size] = 'x';
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, GatewayOwnedPendingCancelUsesZeroRequestId) {
  auto message = Example(trading::MessageType::kOrderUpdate);
  message.update.order.cancel_request_id = {};
  message.update.cancel_request_id = {};
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message output;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  EXPECT_TRUE(output.update.order.cancel_pending);
  EXPECT_EQ(output.update.order.cancel_request_id.value, 0);
}

TEST(TradingProtocol, EveryTruncatedPrefixPreservesDecodeOutput) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(
                  Example(trading::MessageType::kOrderUpdate), bytes, size)
                  .ok());
  trading::Message output;
  output.client_id = 191;
  output.sequence = 198;
  for (std::size_t prefix = 0; prefix < size; ++prefix) {
    EXPECT_EQ(
        trading::DecodeMessage(std::span(bytes).first(prefix), output).code,
        base::ErrorCode::kCorrupt)
        << prefix;
    EXPECT_EQ(output.client_id, 191);
    EXPECT_EQ(output.sequence, 198);
  }
  bytes[size] = std::byte{0};
  EXPECT_FALSE(
      trading::DecodeMessage(std::span(bytes).first(size + 1), output).ok());
}

TEST(TradingProtocol, RejectsMalformedHeaderFields) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(Example(trading::MessageType::kHeartbeat),
                                     bytes, size)
                  .ok());
  const auto original = bytes;
  for (const auto index : {0, 4, 6, 12}) {
    bytes = original;
    bytes[index] = std::byte{0xff};
    EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 0) << index;
  }
  bytes = original;
  bytes[8] = std::byte{31};
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 0);
  bytes = original;
  std::memset(bytes.data() + 24, 0, 8);
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 0);
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(31)), 0);
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(33)), 0);
  bytes = original;
  bytes[8] = std::byte{0};
  bytes[9] = std::byte{0x10};
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 4096);
  bytes[8] = std::byte{1};
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 0);
}

TEST(TradingProtocol, RejectsNoncanonicalPayloadFlagsCountsAndEnums) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  trading::Message output;
  ASSERT_TRUE(trading::EncodeMessage(
                  Example(trading::MessageType::kOrderUpdate), bytes, size)
                  .ok());
  bytes[32] = std::byte{2};  // notify_order must be exactly zero or one.
  EXPECT_FALSE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  ASSERT_TRUE(
      trading::EncodeMessage(Example(trading::MessageType::kHello), bytes, size)
          .ok());
  bytes[41] = std::byte{0};  // Embedded NUL in a declared nonempty token.
  EXPECT_FALSE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  ASSERT_TRUE(trading::EncodeMessage(Example(trading::MessageType::kAccount),
                                     bytes, size)
                  .ok());
  bytes[64] =
      std::byte{65};  // position_count follows revision and three cash values.
  EXPECT_FALSE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  ASSERT_TRUE(trading::EncodeMessage(Example(trading::MessageType::kNewOrder),
                                     bytes, size)
                  .ok());
  bytes[48] = std::byte{0xff};
  EXPECT_FALSE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
}

TEST(TradingProtocol, MaximumReasonAndOpaqueBytesRoundTrip) {
  auto message = Example(trading::MessageType::kOrderUpdate);
  std::string opaque(64, '\0');
  opaque[63] = 'x';
  ASSERT_EQ(message.update.order.broker_order_id.Assign(opaque),
            base::ErrorCode::kOk);
  message.fill.broker_order_id = message.update.order.broker_order_id;
  ASSERT_EQ(message.fill.execution_id.Assign(opaque), base::ErrorCode::kOk);
  message.update.reason.Set(4, std::string(200, 'r'));
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  std::size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message output;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  EXPECT_EQ(output.update.order.broker_order_id.view(), opaque);
  EXPECT_EQ(output.fill.execution_id.view(), opaque);
  EXPECT_EQ(output.update.reason.size, 128);
  EXPECT_TRUE(output.update.reason.truncated);
}

}  // namespace
}  // namespace yquant::trading
