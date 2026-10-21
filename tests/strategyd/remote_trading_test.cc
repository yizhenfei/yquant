#include "strategyd/remote_trading.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "gtest/gtest.h"
#include "strategyd/mock_trading_adapter.h"

namespace yquant::strategyd {
namespace {
using base::ErrorCode;
using tradingd::Message;
using tradingd::MessageType;

class RemoteClock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 1000;
};

// A real loopback stream whose handshake is served on a temporary thread;
// subsequent messages are driven synchronously by each test.
class RemoteTradingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dictionary.source_id = {1};

    dictionary.instrument_count = 1;
    auto& item = dictionary.instruments[0];
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
              ErrorCode::kOk);
    instrument = item.instrument.id;
    item.instrument.venue = model::Venue::kSse;
    std::memcpy(item.instrument.symbol, "600000", model::kSymbolLength);
    item.instrument.price_decimals = 2;

    ASSERT_TRUE(market.Configure(dictionary).ok());
    config.adapter = TradingAdapterKind::kTcp;
    config.remote.client_id = 9;
    std::memcpy(config.remote.token, "client-9", 9);
    config.remote.connect_timeout_ns = 1000000000;
    config.remote.endpoint.port = 0;
    ASSERT_TRUE(listener.Open(config.remote.endpoint).ok());
    config.remote.endpoint.port = listener.port();
    account.revision = 1;
    account.cash = {1000000000, 0, 1000000000};
    std::thread handshake([this] {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      bool accepted = false;
      while (!accepted && std::chrono::steady_clock::now() < deadline) {
        if (!listener.Accept(peer, accepted).ok()) return;
        std::this_thread::yield();
      }
      if (!accepted) return;
      Message hello;
      for (;;) {
        const auto polled = peer.Poll(hello);
        if (polled.state == tradingd::ReceiveState::kMessage) break;
        if (polled.state != tradingd::ReceiveState::kIdle ||
            std::chrono::steady_clock::now() >= deadline)
          return;
        std::this_thread::yield();
      }
      Message welcome;
      welcome.type = MessageType::kWelcome;
      welcome.generation = generation;
      welcome.account = account;
      welcome.instruments = hello.instruments;
      welcome.instrument_count = hello.instrument_count;
      if (!peer.Queue(welcome).ok()) return;
      while (!peer.empty() && std::chrono::steady_clock::now() < deadline) {
        if (!peer.Service().ok()) return;
        std::this_thread::yield();
      }
    });
    const auto initialized = remote.Init(config, dictionary);
    handshake.join();
    ASSERT_TRUE(initialized.ok()) << remote.error_detail();
    remote.SetRunning(true);
  }

  model::ClientOrderId Submit(std::int64_t quantity = 10) {
    model::ClientOrderId id;
    remote.BeginCallback(CallbackPhase::kTick);
    EXPECT_EQ(remote.SubmitOrder(
                  {instrument, model::OrderSide::kBuy, 1001, quantity}, id),
              ErrorCode::kOk);
    remote.EndCallback();
    return id;
  }

  Message Receive() {
    Message message;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto polled = peer.Poll(message);
      if (polled.state == tradingd::ReceiveState::kMessage) return message;
      if (polled.state != tradingd::ReceiveState::kIdle) break;
      std::this_thread::yield();
    }
    ADD_FAILURE() << "peer received no complete message";
    return message;
  }

  void Send(Message message) {
    message.generation = generation;
    ASSERT_TRUE(peer.Queue(message).ok()) << peer.error_detail();
    ASSERT_TRUE(peer.Service().ok()) << peer.error_detail();
  }

  base::Status Poll(TradingDispatch& dispatch, bool& event) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
      auto status = remote.Poll(clock.now, dispatch, event);
      if (!status.ok() || event || std::chrono::steady_clock::now() >= deadline)
        return status;
      std::this_thread::yield();
    }
  }

  // Account/heartbeat/drain messages intentionally have no callback, so a
  // committed-message deadline distinguishes them from an empty stream poll.
  base::Status PollMessage(TradingDispatch& dispatch, bool& event) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
      bool message_event = event;
      const auto status = remote.Poll(clock.now, dispatch, message_event);
      if (!status.ok()) return status;
      if (remote.next_deadline() == clock.now) {
        event = message_event;
        return status;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        ADD_FAILURE() << "client received no complete message";
        return {ErrorCode::kIoError, {}};
      }
      std::this_thread::yield();
    }
  }

  Message Accepted(model::ClientOrderId id) {
    Message result;
    result.type = MessageType::kOrderUpdate;
    result.account = account;
    result.account.revision = 2;
    result.account.cash = {1000000000, 100100000, 899900000};
    result.update.kind = model::OrderUpdateKind::kAccepted;
    EXPECT_TRUE(remote.TryGetOrder(id, result.update.order));
    EXPECT_EQ(result.update.order.broker_order_id.Assign("broker-1"),
              ErrorCode::kOk);
    result.update.order.report_sequence = 1;
    result.update.order.state = model::OrderState::kWorking;
    result.update.order.receive_time_ns = 999999999;
    return result;
  }

  Message Accept(model::ClientOrderId id) {
    auto message = Accepted(id);
    Send(message);
    TradingDispatch dispatch;
    bool event = false;
    EXPECT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
    EXPECT_TRUE(event);
    account = message.account;
    return message;
  }

  void Transmit() {
    ASSERT_TRUE(remote.ProcessRequests(adapter, clock.now).ok())
        << remote.error_detail();
  }

  RemoteClock clock;
  MarketDataConfig dictionary;
  MarketDataImpl market{clock};
  RemoteTrading remote{clock, market};
  MockTradingAdapter adapter;
  TradingConfig config;
  tradingd::Listener listener;
  tradingd::Connection peer;
  tradingd::AccountView account;
  model::InstrumentId instrument;
  static constexpr std::uint64_t generation = 77;
};

TEST_F(RemoteTradingTest, LocalQueueDoesNotReserveAuthoritativeAccount) {
  model::CashSnapshot before;
  ASSERT_TRUE(remote.TryGetCash(before));
  // This is much larger than available account cash; final risk belongs to
  // tradingd and must produce an asynchronous rejection rather than a guess.
  const auto id = Submit(1000);
  EXPECT_EQ(id.value, 1U);
  model::CashSnapshot after;
  ASSERT_TRUE(remote.TryGetCash(after));
  EXPECT_EQ(before, after);
  Transmit();
  const auto message = Receive();
  EXPECT_EQ(message.type, MessageType::kNewOrder);
  EXPECT_EQ(message.order_id, id);
  EXPECT_EQ(message.request.quantity, 1000);
  EXPECT_EQ(message.generation, 77U);
}

TEST_F(RemoteTradingTest, CommitsOrderFillAndAccountBeforeDispatch) {
  const auto id = Submit();
  Transmit();
  ASSERT_EQ(Receive().type, MessageType::kNewOrder);
  auto accepted = Accept(id);
  auto fill = accepted;
  fill.account.revision = 3;
  fill.account.cash = {899900000, 0, 899900000};
  fill.account.position_count = 1;
  fill.account.positions[0] = {instrument, 10, 0, 0};
  fill.update.kind = model::OrderUpdateKind::kFill;
  fill.update.order.report_sequence = 2;
  fill.update.order.state = model::OrderState::kFilled;
  fill.update.order.cumulative_filled_qty = 10;
  fill.update.order.leaves_qty = 0;
  fill.has_fill = true;
  fill.fill.client_order_id = id;
  fill.fill.broker_order_id = fill.update.order.broker_order_id;
  ASSERT_EQ(fill.fill.execution_id.Assign("exec-1"), ErrorCode::kOk);
  fill.fill.instrument_id = instrument;
  fill.fill.side = model::OrderSide::kBuy;
  fill.fill.price = 1001;
  fill.fill.quantity = 10;
  fill.fill.receive_time_ns = 999999999;
  Send(fill);
  TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
  ASSERT_TRUE(event);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_TRUE(dispatch.has_fill);
  EXPECT_EQ(dispatch.update.order.receive_time_ns, 1000U);
  EXPECT_EQ(dispatch.fill.receive_time_ns, 1000U);
  model::CashSnapshot cash;
  ASSERT_TRUE(remote.TryGetCash(cash));
  EXPECT_EQ(cash.available, 899900000);
  model::PositionSnapshot position;
  ASSERT_TRUE(remote.TryGetPosition(instrument, position));
  EXPECT_EQ(position.total_qty, 10);
  model::OrderSnapshot order;
  ASSERT_TRUE(remote.TryGetOrder(id, order));
  EXPECT_EQ(order, dispatch.update.order);
}

TEST_F(RemoteTradingTest, StopsUnsentOrdersLocallyAndWaitsForGatewayAck) {
  const auto id = Submit();
  remote.BeginStopping();
  Transmit();
  EXPECT_EQ(Receive().type, MessageType::kStop);
  ASSERT_TRUE(remote.has_local_updates());
  TradingDispatch dispatch;
  ASSERT_EQ(remote.ApplyLocalUpdate(dispatch), ErrorCode::kOk);
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCanceledBeforeSend);
  EXPECT_EQ(dispatch.update.order.client_order_id, id);
  EXPECT_FALSE(remote.is_drained());
  Message drained;
  drained.type = MessageType::kDrained;
  Send(drained);
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_FALSE(event);
  EXPECT_TRUE(remote.is_drained());
}

TEST_F(RemoteTradingTest, AcceptsGatewayCancellationWithZeroCancelIdOnStop) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto accepted = Accept(id);
  remote.BeginStopping();
  Transmit();
  EXPECT_EQ(Receive().type, MessageType::kStop);
  auto canceled = accepted;
  canceled.update.kind = model::OrderUpdateKind::kCanceled;
  canceled.update.order.report_sequence = 2;
  canceled.update.order.state = model::OrderState::kCanceled;
  canceled.update.order.leaves_qty = 0;
  canceled.account = account;
  canceled.account.revision = 3;
  canceled.account.cash = {1000000000, 0, 1000000000};
  Send(canceled);
  TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
  EXPECT_TRUE(event);
  EXPECT_EQ(dispatch.update.cancel_request_id.value, 0U);
  EXPECT_FALSE(remote.is_drained());
  Message drained;
  drained.type = MessageType::kDrained;
  Send(drained);
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_TRUE(remote.is_drained());
}

TEST_F(RemoteTradingTest, LostConnectionPreservesAttemptedOrderFacts) {
  const auto id = Submit();
  Transmit();
  ASSERT_EQ(Receive().type, MessageType::kNewOrder);
  peer.Close();
  TradingDispatch dispatch;
  bool event = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  base::Status status;
  do {
    status = remote.Poll(clock.now, dispatch, event);
  } while (status.ok() && std::chrono::steady_clock::now() < deadline);
  EXPECT_FALSE(status.ok());
  model::OrderSnapshot order;
  ASSERT_TRUE(remote.TryGetOrder(id, order));
  EXPECT_EQ(order.state, model::OrderState::kPendingNew);
  EXPECT_FALSE(remote.is_drained());
  remote.BeginCallback(CallbackPhase::kTick);
  EXPECT_FALSE(remote.accepting_orders());
  remote.EndCallback();
}

TEST_F(RemoteTradingTest, GatewayCanCancelRunningClientBeforeClosingStream) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto canceled = Accept(id);
  canceled.update.kind = model::OrderUpdateKind::kCanceled;
  canceled.update.order.report_sequence = 2;
  canceled.update.order.state = model::OrderState::kCanceled;
  canceled.update.order.leaves_qty = 0;
  canceled.account.revision = 3;
  canceled.account.cash = {1000000000, 0, 1000000000};
  Send(canceled);
  TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
  EXPECT_TRUE(event);
  EXPECT_EQ(dispatch.update.order.state, model::OrderState::kCanceled);
  EXPECT_EQ(dispatch.update.cancel_request_id.value, 0U);
  peer.Close();
  const auto status = Poll(dispatch, event);
  EXPECT_EQ(status.code, ErrorCode::kIoError);
  model::OrderSnapshot retained;
  ASSERT_TRUE(remote.TryGetOrder(id, retained));
  EXPECT_EQ(retained.state, model::OrderState::kCanceled);
}

TEST_F(RemoteTradingTest, HeartbeatTimeoutEndsSessionAndIdleAdvancesDeadline) {
  TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(remote.Poll(clock.now, dispatch, event).ok());
  EXPECT_FALSE(event);
  ASSERT_TRUE(remote.next_deadline());
  EXPECT_GT(*remote.next_deadline(), clock.now);
  clock.now += config.remote.heartbeat_timeout_ns;
  EXPECT_FALSE(remote.ProcessRequests(adapter, clock.now).ok());
  EXPECT_NE(std::strstr(remote.error_detail(), "heartbeat"), nullptr);
}

TEST_F(RemoteTradingTest, AccountBroadcastCanContainOtherStrategyInstruments) {
  Message message;
  message.type = MessageType::kAccount;
  message.account = account;
  message.account.revision = 2;
  message.account.position_count = 1;
  message.account.positions[0] = {{(2ULL << 32) | 1}, 100, 90, 10};
  Send(message);
  TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_FALSE(event);
  model::PositionSnapshot position;
  EXPECT_TRUE(remote.TryGetPosition(instrument, position));
  EXPECT_EQ(position.total_qty, 0);
  EXPECT_FALSE(remote.TryGetPosition({(2ULL << 32) | 1}, position));
}

TEST_F(RemoteTradingTest, ExactDuplicateDoesNotRepeatCallbacksOrFill) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto accepted = Accept(id);
  accepted.update.order.receive_time_ns = 123456;
  Send(accepted);
  TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok()) << remote.error_detail();
  EXPECT_FALSE(event);
}

struct InvalidRemoteUpdate {
  const char* name;
  void (*mutate)(Message&);
};
class InvalidRemoteTradingUpdateTest
    : public RemoteTradingTest,
      public ::testing::WithParamInterface<InvalidRemoteUpdate> {};

TEST_P(InvalidRemoteTradingUpdateTest,
       PreservesOrderAccountAndOutputOnFailure) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto message = Accepted(id);
  GetParam().mutate(message);
  Send(message);
  model::OrderSnapshot before;
  ASSERT_TRUE(remote.TryGetOrder(id, before));
  model::CashSnapshot cash_before;
  ASSERT_TRUE(remote.TryGetCash(cash_before));
  TradingDispatch dispatch;
  dispatch.update.order.client_order_id = {999};
  bool event = true;
  auto status = PollMessage(dispatch, event);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 999U);
  EXPECT_TRUE(event);
  model::OrderSnapshot after;
  ASSERT_TRUE(remote.TryGetOrder(id, after));
  EXPECT_EQ(before, after);
  model::CashSnapshot cash_after;
  ASSERT_TRUE(remote.TryGetCash(cash_after));
  EXPECT_EQ(cash_before, cash_after);
}

INSTANTIATE_TEST_SUITE_P(
    Validation, InvalidRemoteTradingUpdateTest,
    ::testing::Values(
        InvalidRemoteUpdate{
            "WrongOrder",
            [](Message& value) { value.update.order.client_order_id = {2}; }},
        InvalidRemoteUpdate{
            "ChangedPrice",
            [](Message& value) { ++value.update.order.limit_price; }},
        InvalidRemoteUpdate{
            "SequenceGap",
            [](Message& value) { value.update.order.report_sequence = 2; }},
        InvalidRemoteUpdate{"ChangedQuantity",
                            [](Message& value) {
                              ++value.update.order.original_qty;
                              ++value.update.order.leaves_qty;
                            }},
        InvalidRemoteUpdate{"WrongTransition",
                            [](Message& value) {
                              value.update.order.state =
                                  model::OrderState::kFilled;
                              value.update.order.cumulative_filled_qty = 10;
                              value.update.order.leaves_qty = 0;
                            }},
        InvalidRemoteUpdate{"SuppressedAccept",
                            [](Message& value) { value.notify_order = false; }},
        InvalidRemoteUpdate{
            "AccountMutation",
            [](Message& value) { value.account.revision = 1; }}),
    [](const ::testing::TestParamInfo<InvalidRemoteUpdate>& info) {
      return info.param.name;
    });

}  // namespace
}  // namespace yquant::strategyd
