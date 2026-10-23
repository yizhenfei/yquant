#include <stdint.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "gtest/gtest.h"
#include "strategyd/trading_service_impl.h"

namespace yquant::strategyd {
namespace {
using base::ErrorCode;
using trading::Message;
using trading::MessageType;

class RemoteClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000;
};

// A real loopback stream whose handshake is served on a temporary thread;
// subsequent messages are driven synchronously by each test.
class TradingServiceImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dictionary.source_id = {1};

    dictionary.instrument_count = 2;
    auto& item = dictionary.instruments[0];
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
              ErrorCode::kOk);
    instrument = item.instrument.id;
    item.instrument.venue = model::Venue::kSse;
    std::memcpy(item.instrument.symbol, "600000", model::kSymbolLength);
    item.instrument.price_decimals = 2;

    auto& future = dictionary.instruments[1];
    ASSERT_EQ(model::MakeInstrumentId("SHFE", "rb2701", future.instrument.id),
              ErrorCode::kOk);
    futures_instrument = future.instrument.id;
    future.instrument.venue = model::Venue::kShfe;
    std::memcpy(future.instrument.symbol, "rb2701", 6);
    future.instrument.type = model::InstrumentType::kFutures;
    future.instrument.contract_multiplier = 10;
    future.instrument.margin_rate_ppm = 100000;

    ASSERT_TRUE(market.Configure(dictionary).ok());
    config.client_id = 9;
    std::memcpy(config.token, "client-9", 9);
    config.connect_timeout_ns = 1000000000;
    config.endpoint.port = 0;
    ASSERT_TRUE(listener.Open(config.endpoint).ok());
    config.endpoint.port = listener.port();
    account.revision = 1;
    account.cash = {1000000000, 0, 1000000000};
    ConfigureInitialAccount();
    std::thread handshake([this] {
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(2);
      bool accepted = false;
      while (!accepted && std::chrono::steady_clock::now() < deadline) {
        if (!listener.Accept(peer, accepted).ok()) {
          return;
        }
        std::this_thread::yield();
      }
      if (!accepted) {
        return;
      }
      trading::Message hello;
      for (;;) {
        const auto polled = peer.Poll(hello);
        if (polled.state == trading::ReceiveState::kMessage) {
          break;
        }
        if (polled.state != trading::ReceiveState::kIdle ||
            std::chrono::steady_clock::now() >= deadline) {
          return;
        }
        std::this_thread::yield();
      }
      EXPECT_EQ(hello.instrument_count, 2U);
      EXPECT_EQ(hello.instruments[0].id, instrument);
      Message welcome;
      welcome.type = MessageType::kWelcome;
      welcome.generation = generation;
      welcome.account = account;
      welcome.instruments = hello.instruments;
      welcome.instrument_count = hello.instrument_count;
      if (!peer.Queue(welcome).ok()) {
        return;
      }
      while (!peer.empty() && std::chrono::steady_clock::now() < deadline) {
        if (!peer.Service().ok()) {
          return;
        }
        std::this_thread::yield();
      }
    });
    const auto initialized = remote.Init(config, dictionary);
    handshake.join();
    ASSERT_TRUE(initialized.ok()) << remote.error_detail();
  }

  model::ClientOrderId Submit(int64_t quantity = 10) {
    model::ClientOrderId id;
    remote.SetAcceptingRequests(true);
    EXPECT_EQ(remote.SubmitOrder(
                  {instrument, model::OrderSide::kBuy, 1001, quantity}, id),
              ErrorCode::kOk);
    remote.SetAcceptingRequests(false);
    return id;
  }
  // Derived fixtures may replace the cash welcome with broker-owned futures.
  virtual void ConfigureInitialAccount() {}

  trading::Message Receive() {
    trading::Message message;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto polled = peer.Poll(message);
      if (polled.state == trading::ReceiveState::kMessage) {
        return message;
      }
      if (polled.state != trading::ReceiveState::kIdle) {
        break;
      }
      std::this_thread::yield();
    }
    ADD_FAILURE() << "peer received no complete message";
    return message;
  }

  void Send(trading::Message message) {
    message.generation = generation;
    ASSERT_TRUE(peer.Queue(message).ok()) << peer.error_detail();
    ASSERT_TRUE(peer.Service().ok()) << peer.error_detail();
  }

  base::Status Poll(trading::TradingDispatch& dispatch, bool& event) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
      auto status = remote.Poll(clock.now, dispatch, event);
      if (!status.ok() || event ||
          std::chrono::steady_clock::now() >= deadline) {
        return status;
      }
      std::this_thread::yield();
    }
  }

  // Account/heartbeat/drain messages intentionally have no callback, so a
  // committed-message deadline distinguishes them from an empty stream poll.
  base::Status PollMessage(trading::TradingDispatch& dispatch, bool& event) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    for (;;) {
      bool message_event = event;
      const auto status = remote.Poll(clock.now, dispatch, message_event);
      if (!status.ok()) {
        return status;
      }
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

  trading::Message Accepted(model::ClientOrderId id) {
    trading::Message result;
    result.type = trading::MessageType::kOrderUpdate;
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

  trading::Message Accept(model::ClientOrderId id) {
    auto message = Accepted(id);
    Send(message);
    trading::TradingDispatch dispatch;
    bool event = false;
    EXPECT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
    EXPECT_TRUE(event);
    account = message.account;
    return message;
  }

  void Transmit() {
    ASSERT_TRUE(remote.ProcessRequests(clock.now).ok())
        << remote.error_detail();
  }

  RemoteClock clock;
  md::MarketDataConfig dictionary;
  md::InstrumentScope market;
  TradingServiceImpl remote{clock, market};
  trading::TradingClientConfig config;
  trading::Listener listener;
  trading::Connection peer;
  trading::AccountView account;
  model::InstrumentId instrument;
  model::InstrumentId futures_instrument;
  static constexpr uint64_t generation = 77;
};

class BrokerTradingServiceImplTest : public TradingServiceImplTest {
 protected:
  void ConfigureInitialAccount() override {
    account = {};
    account.revision = 1;
    account.type = model::AccountType::kFutures;
    account.broker_managed = true;
    account.futures = {93, 100, 43, 8, 6, 0, 7, 2};
    account.futures_position_count = 1;
    account.futures_positions[0].instrument_id = futures_instrument;
    account.futures_positions[0].long_today = {2, 0, 2010000000};
    account.futures_positions[0].long_yesterday = {1, 0, 1000000000};
    account.futures_positions[0].unallocated_long_frozen = 1;
  }
};

TEST_F(BrokerTradingServiceImplTest,
       PureBrokerUpdatesPreserveMoneyAndNeverInjectQuotes) {
  EXPECT_TRUE(remote.supports_generic_close());
  model::FuturesAccountSnapshot before;
  ASSERT_TRUE(remote.TryGetFuturesAccount(before));
  EXPECT_EQ(before, account.futures);
  model::QuoteTick quote;
  quote.instrument_id = futures_instrument;
  quote.source_id = {1};
  quote.sequence = 1;
  quote.bid_price = 100;
  quote.ask_price = 101;
  quote.bid_qty = quote.ask_qty = 1;
  quote.flags = 7;
  ASSERT_TRUE(remote.OnQuote(quote).ok());
  Transmit();
  Message unexpected;
  EXPECT_EQ(peer.Poll(unexpected).state, trading::ReceiveState::kIdle);
  Message update;
  update.type = MessageType::kAccount;
  update.account = account;
  update.account.revision = 2;
  update.account.futures.available = 9;
  update.account.futures_positions[0].unallocated_long_frozen = 2;
  Send(update);
  trading::TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_FALSE(event);
  ASSERT_TRUE(remote.TryGetFuturesAccount(before));
  EXPECT_EQ(before.available, 9);
  model::FuturesPositionSnapshot position;
  ASSERT_TRUE(remote.TryGetFuturesPosition(futures_instrument, position));
  EXPECT_EQ(position.unallocated_long_frozen, 2);
}

TEST_F(BrokerTradingServiceImplTest,
       ExternalCancellationBeforeAcceptanceSettlesOrder) {
  remote.SetAcceptingRequests(true);
  model::ClientOrderId id;
  ASSERT_EQ(remote.SubmitOrder({futures_instrument, model::OrderSide::kBuy, 100,
                                1, model::PositionEffect::kOpen},
                               id),
            ErrorCode::kOk);
  remote.SetAcceptingRequests(false);
  Transmit();
  (void)Receive();
  Message canceled;
  canceled.type = MessageType::kOrderUpdate;
  canceled.account = account;
  canceled.update.kind = model::OrderUpdateKind::kCanceled;
  ASSERT_TRUE(remote.TryGetOrder(id, canceled.update.order));
  canceled.update.order.state = model::OrderState::kCanceled;
  canceled.update.order.leaves_qty = 0;
  canceled.update.order.report_sequence = 1;
  Send(canceled);
  trading::TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
  EXPECT_TRUE(event);
  EXPECT_EQ(dispatch.update.cancel_request_id.value, 0);
  EXPECT_EQ(dispatch.update.order.state, model::OrderState::kCanceled);
}

TEST_F(TradingServiceImplTest, LocalQueueDoesNotReserveAuthoritativeAccount) {
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
  EXPECT_EQ(message.type, trading::MessageType::kNewOrder);
  EXPECT_EQ(message.order_id, id);
  EXPECT_EQ(message.request.quantity, 1000);
  EXPECT_EQ(message.generation, 77U);
}

TEST_F(TradingServiceImplTest, CommitsOrderFillAndAccountBeforeDispatch) {
  const auto id = Submit();
  Transmit();
  ASSERT_EQ(Receive().type, trading::MessageType::kNewOrder);
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
  trading::TradingDispatch dispatch;
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

TEST_F(TradingServiceImplTest, StopsUnsentOrdersLocallyAndWaitsForGatewayAck) {
  const auto id = Submit();
  remote.BeginStopping();
  Transmit();
  EXPECT_EQ(Receive().type, trading::MessageType::kStop);
  ASSERT_TRUE(remote.has_ready_event(clock.now));
  trading::TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(remote.Poll(clock.now, dispatch, event).ok());
  ASSERT_TRUE(event);
  EXPECT_TRUE(dispatch.notify_order);
  EXPECT_EQ(dispatch.update.kind, model::OrderUpdateKind::kCanceledBeforeSend);
  EXPECT_EQ(dispatch.update.order.client_order_id, id);
  EXPECT_FALSE(remote.is_drained());
  trading::Message drained;
  drained.type = trading::MessageType::kDrained;
  Send(drained);
  event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_FALSE(event);
  EXPECT_TRUE(remote.is_drained());
}

TEST_F(TradingServiceImplTest,
       AcceptsGatewayCancellationWithZeroCancelIdOnStop) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto accepted = Accept(id);
  remote.BeginStopping();
  Transmit();
  EXPECT_EQ(Receive().type, trading::MessageType::kStop);
  auto canceled = accepted;
  canceled.update.kind = model::OrderUpdateKind::kCanceled;
  canceled.update.order.report_sequence = 2;
  canceled.update.order.state = model::OrderState::kCanceled;
  canceled.update.order.leaves_qty = 0;
  canceled.account = account;
  canceled.account.revision = 3;
  canceled.account.cash = {1000000000, 0, 1000000000};
  Send(canceled);
  trading::TradingDispatch dispatch;
  bool event = false;
  ASSERT_TRUE(Poll(dispatch, event).ok()) << remote.error_detail();
  EXPECT_TRUE(event);
  EXPECT_EQ(dispatch.update.cancel_request_id.value, 0U);
  EXPECT_FALSE(remote.is_drained());
  trading::Message drained;
  drained.type = trading::MessageType::kDrained;
  Send(drained);
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_TRUE(remote.is_drained());
}

TEST_F(TradingServiceImplTest, LostConnectionPreservesAttemptedOrderFacts) {
  const auto id = Submit();
  Transmit();
  ASSERT_EQ(Receive().type, trading::MessageType::kNewOrder);
  peer.Close();
  trading::TradingDispatch dispatch;
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
  remote.SetAcceptingRequests(true);
  EXPECT_FALSE(remote.accepting_orders());
  remote.SetAcceptingRequests(false);
}

TEST_F(TradingServiceImplTest,
       GatewayCanCancelRunningClientBeforeClosingStream) {
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
  trading::TradingDispatch dispatch;
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

TEST_F(TradingServiceImplTest,
       RequestPermissionBlocksTransportAndStoppingStaysClosed) {
  const auto id = Submit();
  remote.SetAcceptingRequests(true);
  EXPECT_EQ(remote.ProcessRequests(clock.now).code, ErrorCode::kInvalidState);
  trading::TradingDispatch dispatch;
  dispatch.update.order.client_order_id = {99};
  bool event = true;
  EXPECT_EQ(remote.Poll(clock.now, dispatch, event).code,
            ErrorCode::kInvalidState);
  EXPECT_EQ(dispatch.update.order.client_order_id.value, 99U);
  EXPECT_TRUE(event);
  remote.SetAcceptingRequests(false);
  remote.BeginStopping();
  remote.SetAcceptingRequests(true);
  EXPECT_FALSE(remote.accepting_orders());
  EXPECT_EQ(remote.CancelOrder(id), ErrorCode::kInvalidState);
}

TEST_F(TradingServiceImplTest,
       HeartbeatTimeoutEndsSessionAndIdleAdvancesDeadline) {
  trading::TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(remote.Poll(clock.now, dispatch, event).ok());
  EXPECT_FALSE(event);
  ASSERT_TRUE(remote.next_deadline());
  EXPECT_GT(*remote.next_deadline(), clock.now);
  clock.now += config.heartbeat_timeout_ns;
  EXPECT_FALSE(remote.ProcessRequests(clock.now).ok());
  EXPECT_NE(std::strstr(remote.error_detail(), "heartbeat"), nullptr);
}

TEST_F(TradingServiceImplTest,
       AccountBroadcastCanContainOtherStrategyInstruments) {
  trading::Message message;
  message.type = trading::MessageType::kAccount;
  message.account = account;
  message.account.revision = 2;
  message.account.position_count = 1;
  message.account.positions[0] = {{(2ULL << 32) | 1}, 100, 90, 10};
  Send(message);
  trading::TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok());
  EXPECT_FALSE(event);
  model::PositionSnapshot position;
  EXPECT_TRUE(remote.TryGetPosition(instrument, position));
  EXPECT_EQ(position.total_qty, 0);
  EXPECT_FALSE(remote.TryGetPosition({(2ULL << 32) | 1}, position));
}

TEST_F(TradingServiceImplTest, ExactDuplicateDoesNotRepeatCallbacksOrFill) {
  const auto id = Submit();
  Transmit();
  (void)Receive();
  auto accepted = Accept(id);
  accepted.update.order.receive_time_ns = 123456;
  Send(accepted);
  trading::TradingDispatch dispatch;
  bool event = true;
  ASSERT_TRUE(PollMessage(dispatch, event).ok()) << remote.error_detail();
  EXPECT_FALSE(event);
}

struct InvalidRemoteUpdate {
  const char* name;
  void (*mutate)(trading::Message&);
};
class InvalidTradingServiceImplUpdateTest
    : public TradingServiceImplTest,
      public ::testing::WithParamInterface<InvalidRemoteUpdate> {};

TEST_P(InvalidTradingServiceImplUpdateTest,
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
  trading::TradingDispatch dispatch;
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
    Validation, InvalidTradingServiceImplUpdateTest,
    ::testing::Values(
        InvalidRemoteUpdate{"WrongOrder",
                            [](trading::Message& value) {
                              value.update.order.client_order_id = {2};
                            }},
        InvalidRemoteUpdate{
            "ChangedPrice",
            [](trading::Message& value) { ++value.update.order.limit_price; }},
        InvalidRemoteUpdate{"SequenceGap",
                            [](trading::Message& value) {
                              value.update.order.report_sequence = 2;
                            }},
        InvalidRemoteUpdate{"ChangedQuantity",
                            [](trading::Message& value) {
                              ++value.update.order.original_qty;
                              ++value.update.order.leaves_qty;
                            }},
        InvalidRemoteUpdate{"WrongTransition",
                            [](trading::Message& value) {
                              value.update.order.state =
                                  model::OrderState::kFilled;
                              value.update.order.cumulative_filled_qty = 10;
                              value.update.order.leaves_qty = 0;
                            }},
        InvalidRemoteUpdate{
            "SuppressedAccept",
            [](trading::Message& value) { value.notify_order = false; }},
        InvalidRemoteUpdate{
            "AccountMutation",
            [](trading::Message& value) { value.account.revision = 1; }}),
    [](const ::testing::TestParamInfo<InvalidRemoteUpdate>& info) {
      return info.param.name;
    });

TEST_F(TradingServiceImplTest, FuturesQueriesPreserveOutputsWithMixedCatalog) {
  model::FuturesAccountSnapshot account_out;
  account_out.balance = 47;
  const auto account_before = account_out;
  model::FuturesPositionSnapshot position_out;
  position_out.instrument_id = futures_instrument;
  position_out.long_today.quantity = 3;
  const auto position_before = position_out;
  EXPECT_FALSE(remote.TryGetFuturesAccount(account_out));
  EXPECT_EQ(account_out, account_before);
  EXPECT_FALSE(remote.TryGetFuturesPosition(futures_instrument, position_out));
  EXPECT_EQ(position_out, position_before);
  model::PositionSnapshot equity_out{instrument, 17, 13, 4};
  const auto equity_before = equity_out;
  EXPECT_FALSE(remote.TryGetPosition(futures_instrument, equity_out));
  EXPECT_EQ(equity_out, equity_before);
}

TEST_F(TradingServiceImplTest,
       TimerRequestsRejectFuturesEffectsWithoutConsumingId) {
  model::ClientOrderId id{77};
  remote.SetAcceptingRequests(true);
  EXPECT_TRUE(remote.accepting_orders());
  model::OrderRequest request{instrument, model::OrderSide::kBuy, 1001, 10,
                              model::PositionEffect::kOpen};
  EXPECT_EQ(remote.SubmitOrder(request, id), ErrorCode::kInvalidArgument);
  EXPECT_EQ(id.value, 77U);
  request.instrument_id = futures_instrument;
  request.limit_price = 3201;
  EXPECT_EQ(remote.SubmitOrder(request, id), ErrorCode::kUnsupported);
  EXPECT_EQ(id.value, 77U);
  request.instrument_id = instrument;
  request.limit_price = 1001;
  request.position_effect = model::PositionEffect::kNone;
  ASSERT_EQ(remote.SubmitOrder(request, id), ErrorCode::kOk);
  EXPECT_EQ(id.value, 1U);
  remote.SetAcceptingRequests(false);
  ASSERT_TRUE(remote.ProcessRequests(clock.now).ok());
  const auto sent = Receive();
  EXPECT_EQ(sent.type, MessageType::kNewOrder);
  EXPECT_EQ(sent.request.position_effect, model::PositionEffect::kNone);
  EXPECT_EQ(sent.request.instrument_id, instrument);
}

}  // namespace
}  // namespace yquant::strategyd
