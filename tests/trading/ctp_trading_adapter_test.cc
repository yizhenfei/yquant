#include "yquant/trading/ctp_trading_adapter.h"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "gtest/gtest.h"
#include "yquant/trading/connection.h"

namespace yquant::trading {
namespace {

// A real loopback wire peer validates the public adapter boundary,
// independently of the CTP SDK and without creating an external trading
// connection.
class BridgePeer {
 public:
  ~BridgePeer() {
    stop_requested_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
      thread_.join();
    }
  }
  bool Open() { return listener_.Open({"127.0.0.1", 0}).ok(); }
  uint16_t port() const { return listener_.port(); }
  void Start(bool mismatched_report = false) {
    thread_ = std::thread([this, mismatched_report] {
      Connection connection;
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(5);
      bool accepted = false;
      while (!stop_requested_.load(std::memory_order_relaxed) &&
             std::chrono::steady_clock::now() < deadline) {
        if (!accepted) {
          if (!listener_.Accept(connection, accepted).ok()) {
            return;
          }
          if (!accepted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
          }
        }
        Message input;
        const auto received = connection.Poll(input);
        if (received.state == ReceiveState::kClosed ||
            received.state == ReceiveState::kError) {
          return;
        }
        if (received.state == ReceiveState::kIdle) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        Message response;
        response.generation = 42;
        response.account.type = model::AccountType::kFutures;
        response.account.broker_managed = true;
        response.account.revision = 1;
        // Actual broker fields need not obey the local simulator's equation.
        response.account.futures = {1000, 1200, 900, 100, 50, 30, 200, 20};
        if (input.type == MessageType::kHello) {
          if (std::strcmp(input.token, "bridge-test") != 0) {
            return;
          }
          response.type = MessageType::kWelcome;
          response.instruments = input.instruments;
          response.instrument_count = input.instrument_count;
        } else if (input.type == MessageType::kNewOrder) {
          response.type = MessageType::kOrderUpdate;
          response.account.revision = 2;
          response.account.futures.available = 777;
          auto& order = response.update.order;
          order.client_order_id = input.order_id;
          order.instrument_id = input.request.instrument_id;
          order.side = input.request.side;
          order.limit_price = input.request.limit_price;
          order.original_qty = input.request.quantity;
          order.leaves_qty = input.request.quantity;
          order.position_effect = input.request.position_effect;
          order.state = model::OrderState::kWorking;
          order.report_sequence = 1;
          (void)order.broker_order_id.Assign("20261022/1/1/123");
          if (mismatched_report) {
            ++order.limit_price;
          }
          response.update.kind = model::OrderUpdateKind::kAccepted;
        } else if (input.type == MessageType::kCancelOrder) {
          response.type = MessageType::kOrderUpdate;
          response.account.revision = 3;
          auto& order = response.update.order;
          order.client_order_id = input.order_id;
          order.instrument_id = instrument_;
          order.side = model::OrderSide::kBuy;
          order.limit_price = 106785;
          order.original_qty = 1;
          order.position_effect = model::PositionEffect::kOpen;
          order.state = model::OrderState::kCanceled;
          order.report_sequence = 2;
          (void)order.broker_order_id.Assign("20261022/1/1/123");
          response.update.kind = model::OrderUpdateKind::kCanceled;
          response.update.cancel_request_id = input.cancel_id;
        } else if (input.type == MessageType::kStop) {
          // A final broker query can outlive a heartbeat interval. The client
          // must read its result without writing into the expected close.
          const auto finish =
              std::chrono::steady_clock::now() + std::chrono::milliseconds(320);
          while (!stop_requested_.load(std::memory_order_relaxed) &&
                 std::chrono::steady_clock::now() < finish) {
            Message extra;
            const auto polled = connection.Poll(extra);
            if (polled.state == ReceiveState::kMessage &&
                extra.type == MessageType::kHeartbeat) {
              heartbeat_after_stop_.store(true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
          response.type = MessageType::kAccount;
          response.account.revision = 4;
          response.account.futures.available = 666;
          if (!connection.Queue(response).ok()) {
            return;
          }
          response.type = MessageType::kDrained;
          if (!connection.Queue(response).ok()) {
            return;
          }
          while (!connection.empty() &&
                 !stop_requested_.load(std::memory_order_relaxed)) {
            if (!connection.Service().ok()) {
              return;
            }
          }
          return;
        } else if (input.type == MessageType::kHeartbeat) {
          response.type = MessageType::kHeartbeat;
        } else {
          return;
        }
        if (!connection.Queue(response).ok()) {
          return;
        }
      }
    });
  }
  void SetInstrument(model::InstrumentId id) { instrument_ = id; }
  bool heartbeat_after_stop() const { return heartbeat_after_stop_.load(); }

 private:
  Listener listener_;
  model::InstrumentId instrument_;
  std::atomic<bool> heartbeat_after_stop_{false};
  // Destruction requests exit and joins before releasing the owned listener.
  std::atomic<bool> stop_requested_{false};
  std::thread thread_;
};

md::MarketDataConfig Dictionary() {
  md::MarketDataConfig config;
  config.source_id = {1};
  config.instrument_count = 1;
  auto& d = config.instruments[0].instrument;
  (void)model::MakeInstrumentId("CFFEX", "TF2612", d.id);
  d.venue = model::Venue::kCffex;
  std::memcpy(d.symbol, "TF2612", 7);
  d.type = model::InstrumentType::kFutures;
  d.price_decimals = 3;
  d.price_tick = 5;
  d.quantity_step = 1;
  d.contract_multiplier = 10000;
  d.margin_rate_ppm = 20000;
  return config;
}

TradingConfig Config(uint16_t port) {
  TradingConfig config;
  config.adapter = TradingAdapterKind::kCtp;
  config.initial_account.type = model::AccountType::kFutures;
  config.ctp.port = port;
  config.ctp.connect_timeout_ns = 2000000000;
  std::strcpy(config.ctp.token, "bridge-test");
  return config;
}

TradingPollResult Wait(CtpTradingAdapter& adapter, TradingEvent& event) {
  for (size_t attempt = 0; attempt < 1000; ++attempt) {
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    const auto polled = adapter.Poll(now, event);
    if (polled.state != TradingPollState::kIdle) {
      return polled;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return {TradingPollState::kError, {base::ErrorCode::kIoError, {}}};
}

TEST(CtpTradingAdapterTest, BrokerAccountReportsAndCancelUseActualWire) {
  md::InstrumentScope scope;
  const auto dictionary = Dictionary();
  ASSERT_TRUE(scope.Configure(dictionary).ok());
  BridgePeer peer;
  ASSERT_TRUE(peer.Open());
  peer.SetInstrument(scope[0].id);
  peer.Start();
  CtpTradingAdapter adapter(scope);
  ASSERT_TRUE(adapter.Prepare(Config(peer.port())).ok());
  ASSERT_TRUE(adapter.Start(0).ok()) << adapter.error_detail();
  BrokerAccountSnapshot account;
  ASSERT_TRUE(adapter.ReadBrokerAccountSnapshot(account).ok());
  EXPECT_EQ(account.account.available, 900);
  EXPECT_EQ(account.account.equity, 1200);
  const model::OrderRequest request{scope[0].id, model::OrderSide::kBuy, 106785,
                                    1, model::PositionEffect::kOpen};
  ASSERT_TRUE(adapter.SendOrder({10}, request, 0).ok());
  EXPECT_FALSE(adapter.is_drained());
  TradingEvent event;
  ASSERT_EQ(Wait(adapter, event).state, TradingPollState::kEvent)
      << adapter.error_detail();
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kAccepted);
  EXPECT_EQ(event.client_order_id.value, 10);
  ASSERT_TRUE(adapter.ReadBrokerAccountSnapshot(account).ok());
  EXPECT_EQ(account.account.available, 777);
  ASSERT_TRUE(adapter.SendCancel({10}, event.broker_order_id, {7}, 0).ok());
  ASSERT_EQ(Wait(adapter, event).state, TradingPollState::kEvent);
  EXPECT_EQ(event.kind, model::OrderUpdateKind::kCanceled);
  EXPECT_EQ(event.cancel_request_id.value, 7);
  EXPECT_TRUE(adapter.is_drained());
  adapter.BeginStopping();
  EXPECT_FALSE(adapter.is_drained());
  ASSERT_EQ(Wait(adapter, event).state, TradingPollState::kAccount);
  ASSERT_TRUE(adapter.ReadBrokerAccountSnapshot(account).ok());
  EXPECT_EQ(account.revision, 4);
  EXPECT_EQ(account.account.available, 666);
  EXPECT_FALSE(adapter.is_drained());
  for (size_t i = 0; i < 1000 && !adapter.is_drained(); ++i) {
    const auto now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    EXPECT_NE(adapter.Poll(now, event).state, TradingPollState::kError);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(adapter.is_drained());
  EXPECT_FALSE(peer.heartbeat_after_stop());
  EXPECT_TRUE(adapter.Stop().ok()) << adapter.error_detail();
}

TEST(CtpTradingAdapterTest, MismatchedBrokerOrderFailsWithoutChangingOutput) {
  md::InstrumentScope scope;
  ASSERT_TRUE(scope.Configure(Dictionary()).ok());
  BridgePeer peer;
  ASSERT_TRUE(peer.Open());
  peer.Start(true);
  CtpTradingAdapter adapter(scope);
  ASSERT_TRUE(adapter.Prepare(Config(peer.port())).ok());
  ASSERT_TRUE(adapter.Start(0).ok());
  ASSERT_TRUE(adapter
                  .SendOrder({10},
                             {scope[0].id, model::OrderSide::kBuy, 106785, 1,
                              model::PositionEffect::kOpen},
                             0)
                  .ok());
  TradingEvent output;
  output.report_sequence = 999;
  EXPECT_EQ(Wait(adapter, output).state, TradingPollState::kError);
  EXPECT_EQ(output.report_sequence, 999);
  EXPECT_FALSE(adapter.is_drained());
  EXPECT_FALSE(adapter.Stop().ok());
}

}  // namespace
}  // namespace yquant::trading
