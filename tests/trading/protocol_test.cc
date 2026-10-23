#include "yquant/trading/protocol.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <string>

#include "gtest/gtest.h"
#include "yquant/base/format.h"

namespace yquant::trading {
namespace {

model::InstrumentId Instrument(uint32_t symbol = 600000) {
  return {(static_cast<uint64_t>(model::Venue::kSse) << 32) | symbol};
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
  message.client_order_id = {17};
  message.request = {Instrument(), model::OrderSide::kBuy, 120, 100};
  message.cancel_id = {9};
  EXPECT_EQ(message.adapter_order_id.Assign({"B\0O", 3}), base::ErrorCode::kOk);
  auto& order = message.update.order;
  order.client_order_id = message.client_order_id;
  order.adapter_order_id = message.adapter_order_id;
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
  message.fill.client_order_id = message.client_order_id;
  message.fill.adapter_order_id = message.adapter_order_id;
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
  size_t size = 123;
  EXPECT_FALSE(trading::EncodeMessage(message, bytes, size).ok());
  EXPECT_EQ(size, 123);
  EXPECT_EQ(bytes, original);
}

class ProtocolRoundTripTest
    : public testing::TestWithParam<trading::MessageType> {};

TEST_P(ProtocolRoundTripTest, CanonicalRoundTripPreservesOwnedValues) {
  const auto original = Example(GetParam());
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
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
  size_t reencoded_size = 0;
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
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  ASSERT_EQ(size, 32);
  EXPECT_EQ(std::memcmp(bytes.data(), "YQTR", 4), 0);
  EXPECT_EQ(bytes[4], std::byte{4});
  EXPECT_EQ(bytes[5], std::byte{0});
  EXPECT_EQ(bytes[8], std::byte{32});
  for (size_t index = 0; index < 8; ++index) {
    EXPECT_EQ(bytes[16 + index], static_cast<std::byte>(index + 1));
    EXPECT_EQ(bytes[24 + index], static_cast<std::byte>(index + 17));
  }
}

TEST(TradingProtocol, DictionaryAndAccountAcceptTheirFullIndependentBounds) {
  auto message = Example(trading::MessageType::kWelcome);
  message.instrument_count = 64;
  message.account.position_count = 64;
  for (size_t index = 0; index < 64; ++index) {
    message.instruments[index] = {Instrument(static_cast<uint32_t>(index)),
                                  static_cast<uint8_t>(index % 7)};
    message.account.positions[index] = {message.instruments[index].id, 100, 80,
                                        20};
  }
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
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
  message.instrument_count = std::numeric_limits<size_t>::max();
  message.account.position_count = std::numeric_limits<size_t>::max();
  message.request.quantity = -1;
  std::memset(message.token, 'x', sizeof(message.token));
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
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
  size_t size = 123;
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
  message.instruments[0] = {{0xff00000001ULL}, 4};  // Unknown venue.
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, RejectsInvalidOrdersAndUnscaledNotionalOverflow) {
  auto message = Example(trading::MessageType::kNewOrder);
  message.client_order_id.value = 0;
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kNewOrder);
  message.request.side = static_cast<model::OrderSide>(3);
  ExpectEncodingRejected(message);
  message = Example(trading::MessageType::kNewOrder);
  message.request.limit_price = 0;
  ExpectEncodingRejected(message);
  message.request.limit_price = -1;
  ExpectEncodingRejected(message);
  message.request.limit_price = std::numeric_limits<int64_t>::max();
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
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message output;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  EXPECT_TRUE(output.update.order.cancel_pending);
  EXPECT_EQ(output.update.order.cancel_request_id.value, 0);
}

TEST(TradingProtocol, EveryTruncatedPrefixPreservesDecodeOutput) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(
                  Example(trading::MessageType::kOrderUpdate), bytes, size)
                  .ok());
  trading::Message output;
  output.client_id = 191;
  output.sequence = 198;
  for (size_t prefix = 0; prefix < size; ++prefix) {
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
  size_t size = 0;
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
  bytes[9] = std::byte{0x40};
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 16384);
  bytes[8] = std::byte{1};
  EXPECT_EQ(trading::FrameSize(std::span(bytes).first(32)), 0);
}

TEST(TradingProtocol, RejectsNoncanonicalPayloadFlagsCountsAndEnums) {
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
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
  bytes[66] =
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
  ASSERT_EQ(message.update.order.adapter_order_id.Assign(opaque),
            base::ErrorCode::kOk);
  message.fill.adapter_order_id = message.update.order.adapter_order_id;
  ASSERT_EQ(message.fill.execution_id.Assign(opaque), base::ErrorCode::kOk);
  message.update.reason.Set(4, std::string(200, 'r'));
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message output;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), output).ok());
  EXPECT_EQ(output.update.order.adapter_order_id.view(), opaque);
  EXPECT_EQ(output.fill.execution_id.view(), opaque);
  EXPECT_EQ(output.update.reason.size, 128);
  EXPECT_TRUE(output.update.reason.truncated);
}

TEST(TradingProtocol, VersionOneRejectsPositionEffectsWithoutDroppingFields) {
  for (const auto effect :
       {model::PositionEffect::kOpen, model::PositionEffect::kCloseToday,
        model::PositionEffect::kCloseYesterday}) {
    auto request = Example(MessageType::kNewOrder);
    request.request.position_effect = effect;
    ExpectEncodingRejected(request);
    auto order = Example(MessageType::kOrderUpdate);
    order.update.order.position_effect = effect;
    ExpectEncodingRejected(order);
    auto fill = Example(MessageType::kOrderUpdate);
    fill.fill.position_effect = effect;
    ExpectEncodingRejected(fill);
  }
}

TEST(TradingProtocol, DecodesLegacyCashAccountFramesForJournalRecovery) {
  std::array<std::byte, 66> frame{};
  size_t offset = 0;
  const auto put = [&](uint64_t value, size_t width) {
    for (size_t i = 0; i < width; ++i) {
      frame[offset++] = std::byte(value & 255);
      value >>= 8;
    }
  };
  put(0x52545159, 4);
  put(1, 2);
  put(6, 2);
  put(66, 4);
  put(0, 4);
  put(7, 8);
  put(1, 8);
  put(9, 8);
  put(100, 8);
  put(10, 8);
  put(90, 8);
  put(0, 2);
  trading::Message decoded;
  ASSERT_TRUE(trading::DecodeMessage(frame, decoded).ok());
  EXPECT_EQ(decoded.type, trading::MessageType::kAccount);
  EXPECT_EQ(decoded.account.type, model::AccountType::kCash);
  EXPECT_EQ(decoded.account.cash, (model::CashSnapshot{100, 10, 90}));
  EXPECT_EQ(decoded.account.revision, 9U);
}

TEST(TradingProtocol, FuturesPositionEffectAndAccountSurviveRoundTrip) {
  auto message = Example(trading::MessageType::kOrderUpdate);
  model::InstrumentId id;
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", id),
            base::ErrorCode::kOk);
  message.update.order.instrument_id = id;
  message.update.order.position_effect = model::PositionEffect::kCloseToday;
  message.fill.instrument_id = id;
  message.fill.position_effect = model::PositionEffect::kCloseToday;
  message.account = {};
  message.account.revision = 4;
  message.account.type = model::AccountType::kFutures;
  message.account.futures = {100, 95, 75, 10, 10, -5, -5, 0};
  message.account.futures_position_count = 1;
  auto& position = message.account.futures_positions[0];
  position.instrument_id = id;
  position.long_today = {90, 90, 10000};
  position.margin = 10;
  position.unrealized_pnl = -5;
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  trading::Message decoded;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.update.order, message.update.order);
  EXPECT_EQ(decoded.fill.position_effect, message.fill.position_effect);
  EXPECT_EQ(decoded.fill.execution_id, message.fill.execution_id);
  EXPECT_EQ(decoded.fill.price, message.fill.price);
  EXPECT_EQ(decoded.fill.quantity, message.fill.quantity);
  EXPECT_EQ(decoded.account.futures, message.account.futures);
  EXPECT_EQ(decoded.account.futures_positions[0], position);
  position.long_today.frozen_qty = 91;
  ExpectEncodingRejected(message);
  position.long_today.frozen_qty = 90;
  message.account.futures.available = 76;
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, CompleteFuturesDictionaryFitsWithinOneFrame) {
  trading::Message message;
  message.type = trading::MessageType::kWelcome;
  message.sequence = 1;
  message.account.type = model::AccountType::kFutures;
  message.account.revision = 1;
  message.instrument_count = message.account.futures_position_count = 64;
  for (size_t i = 0; i < 64; ++i) {
    char symbol[8];
    ::yquant::base::FormatTo(symbol, sizeof(symbol), "{}27{:02}",
                             char('a' + i / 12), unsigned(i % 12 + 1));
    model::InstrumentId id;
    ASSERT_EQ(model::MakeInstrumentId("DCE", symbol, id), base::ErrorCode::kOk);
    message.instruments[i].id = id;
    message.account.futures_positions[i].instrument_id = id;
  }
  std::array<std::byte, trading::kMaxFrameSize> bytes;
  size_t size = 0;
  ASSERT_TRUE(trading::EncodeMessage(message, bytes, size).ok());
  EXPECT_GT(size, 4096U);
  EXPECT_LE(size, 16384U);
  trading::Message decoded;
  ASSERT_TRUE(
      trading::DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.account.futures_position_count, 64U);
  EXPECT_EQ(decoded.account.futures_positions[63],
            message.account.futures_positions[63]);
}

TEST(TradingProtocol, BrokerAccountAndUnallocatedFreezesRoundTrip) {
  auto message = Example(MessageType::kAccount);
  message.account = {};
  message.account.type = model::AccountType::kFutures;
  message.account.broker_managed = true;
  message.account.revision = 1;
  message.account.futures = {93, 100, 43, 8, 6, 0, 7, 2};
  message.account.futures_position_count = 1;
  auto& p = message.account.futures_positions[0];
  ASSERT_EQ(model::MakeInstrumentId("DCE", "a2701", p.instrument_id),
            base::ErrorCode::kOk);
  p.long_today = {2, 1, 2010000000};
  p.long_yesterday = {1, 0, 1000000000};
  p.unallocated_long_frozen = 1;
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t size = 0;
  ASSERT_TRUE(EncodeMessage(message, bytes, size).ok());
  EXPECT_EQ(bytes[4], std::byte{4});
  Message decoded;
  ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_TRUE(decoded.account.broker_managed);
  EXPECT_EQ(decoded.account.futures, message.account.futures);
  EXPECT_EQ(decoded.account.futures_positions[0], p);
  p.unallocated_long_frozen = 3;
  ExpectEncodingRejected(message);
  p.unallocated_long_frozen = 0;
  message.account.broker_managed = false;
  ExpectEncodingRejected(message);
  message = Example(MessageType::kAccount);
  message.account.broker_managed = true;
  ExpectEncodingRejected(message);
}

TEST(TradingProtocol, VersionTwoCashAccountDefaultsToLocalManagement) {
  auto message = Example(MessageType::kAccount);
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t size = 0;
  ASSERT_TRUE(EncodeMessage(message, bytes, size).ok());
  // v2 has the account type at offset 40 but no following management byte.
  std::memmove(bytes.data() + 41, bytes.data() + 42, size - 42);
  --size;
  bytes[4] = std::byte{2};
  for (size_t i = 0; i < 4; ++i) {
    bytes[8 + i] = std::byte((size >> (8 * i)) & 255);
  }
  Message decoded;
  ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_FALSE(decoded.account.broker_managed);
  EXPECT_EQ(decoded.account.cash, message.account.cash);
}

TEST(TradingProtocol, GenericCloseUsesBrokerVenuesAndPreservesEffect) {
  auto request = Example(MessageType::kNewOrder);
  ASSERT_EQ(
      model::MakeInstrumentId("DCE", "a2701", request.request.instrument_id),
      base::ErrorCode::kOk);
  request.request.position_effect = model::PositionEffect::kClose;
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t size = 0;
  ASSERT_TRUE(EncodeMessage(request, bytes, size).ok());
  Message decoded;
  ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.request.position_effect, model::PositionEffect::kClose);
  ASSERT_EQ(
      model::MakeInstrumentId("SHFE", "rb2701", request.request.instrument_id),
      base::ErrorCode::kOk);
  ExpectEncodingRejected(request);
}

TEST(TradingProtocol, VersionTwoFuturesDefaultsUnallocatedFreezesToZero) {
  auto message = Example(MessageType::kAccount);
  message.account = {};
  message.account.revision = 1;
  message.account.type = model::AccountType::kFutures;
  message.account.futures_position_count = 1;
  auto& position = message.account.futures_positions[0];
  ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", position.instrument_id),
            base::ErrorCode::kOk);
  position.long_today = {2, 1, 2010000000};
  std::array<std::byte, trading::kMaxFrameSize> bytes{};
  size_t size = 0;
  ASSERT_TRUE(EncodeMessage(message, bytes, size).ok());
  // Drop v3's two trailing unallocated freezes and its account management byte.
  size -= 16;
  std::memmove(bytes.data() + 41, bytes.data() + 42, size - 42);
  --size;
  bytes[4] = std::byte{2};
  for (size_t i = 0; i < 4; ++i) {
    bytes[8 + i] = std::byte((size >> (8 * i)) & 255);
  }
  Message decoded;
  ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_FALSE(decoded.account.broker_managed);
  EXPECT_EQ(decoded.account.futures_positions[0], position);
}

TEST(TradingProtocol,
     StrategyRequestIdentityIsDistinctFromAdapterClientIdentity) {
  for (const auto type : {MessageType::kNewOrder, MessageType::kCancelOrder}) {
    auto message = Example(type);
    message.client_order_id = {};
    message.strategy_order_id = {7};
    std::array<std::byte, kMaxFrameSize> bytes;
    size_t size = 0;
    ASSERT_TRUE(EncodeMessage(message, bytes, size).ok());
    Message decoded;
    ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
    EXPECT_EQ(decoded.strategy_order_id.value, 7U);
    EXPECT_EQ(decoded.client_order_id.value, 0U);
    message.client_order_id = {19};
    ExpectEncodingRejected(message);
    message.client_order_id = {};
    message.strategy_order_id = {};
    ExpectEncodingRejected(message);
  }
}

TEST(TradingProtocol,
     ReportsPreserveBothOrderIdentitiesAndValidateFillRouting) {
  auto message = Example(MessageType::kOrderUpdate);
  message.update.order.strategy_order_id = {3};
  message.fill.strategy_order_id = {3};
  std::array<std::byte, kMaxFrameSize> bytes;
  size_t size = 0;
  ASSERT_TRUE(EncodeMessage(message, bytes, size).ok());
  Message decoded;
  ASSERT_TRUE(DecodeMessage(std::span(bytes).first(size), decoded).ok());
  EXPECT_EQ(decoded.update.order.strategy_order_id.value, 3U);
  EXPECT_EQ(decoded.update.order.client_order_id.value, 17U);
  EXPECT_EQ(decoded.fill.strategy_order_id.value, 3U);
  EXPECT_EQ(decoded.fill.client_order_id.value, 17U);
  message.fill.strategy_order_id = {4};
  ExpectEncodingRejected(message);
}

}  // namespace
}  // namespace yquant::trading
