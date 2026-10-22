#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "strategy_loop_test_support.h"
#include "strategyd/strategy_runner.h"
#include "test_config_file.h"
#include "yquant/trading/connection.h"

namespace yquant::strategyd {
namespace {

using trading::Message;
using trading::MessageType;
constexpr std::uint64_t kGeneration = 77;
constexpr std::int64_t kInitialCash = 1000000000;

// Stops only after the peer has acknowledged cancellation. The wall deadline
// bounds failure paths; yielding lets the real socket peer make progress.
class RemoteTimerEnvironment final : public TestLoopEnvironment {
 public:
  explicit RemoteTimerEnvironment(const std::atomic<bool>& canceled) noexcept
      : canceled_(canceled),
        deadline_(std::chrono::steady_clock::now() + std::chrono::seconds(3)) {}

  base::Status CheckStop(bool& out) noexcept override {
    out = canceled_.load() || std::chrono::steady_clock::now() >= deadline_;
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t>) noexcept override {
    std::this_thread::yield();
    return {};
  }

 private:
  const std::atomic<bool>& canceled_;
  const std::chrono::steady_clock::time_point deadline_;
};

// A protocol peer exercises the runner's actual remote transport. The only
// generated facts are acceptance, confirmed cancellation and drain completion.
// All mutable socket/account state belongs to the peer thread; main reads it
// only after joining. Cancellation also aborts every wait on a failed runner.
class TimerPeer {
 public:
  bool Open() {
    trading::Endpoint endpoint;
    endpoint.port = 0;
    return listener_.Open(endpoint).ok();
  }
  std::uint16_t port() const { return listener_.port(); }
  void Abort() noexcept { aborted_.store(true); }
  const std::atomic<bool>& canceled() const { return canceled_; }

  void Serve() {
    deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool accepted = false;
    while (!accepted && Active()) {
      if (!listener_.Accept(connection_, accepted).ok()) return;
      std::this_thread::yield();
    }
    if (!accepted) return;
    Message hello;
    if (!Receive(hello) || hello.type != MessageType::kHello ||
        hello.client_id != 19 || hello.instrument_count != 1)
      return;
    Message welcome;
    welcome.type = MessageType::kWelcome;
    welcome.instruments = hello.instruments;
    welcome.instrument_count = hello.instrument_count;
    welcome.account.revision = 1;
    welcome.account.cash = {kInitialCash, 0, kInitialCash};
    if (!Send(welcome)) return;
    Message request;
    if (!ReceiveBusiness(request) || request.type != MessageType::kNewOrder)
      return;
    ++orders;
    Message report;
    report.type = MessageType::kOrderUpdate;
    report.account = welcome.account;
    report.account.revision = 2;
    const auto reserved =
        request.request.limit_price * request.request.quantity * 10000;
    report.account.cash = {kInitialCash, reserved, kInitialCash - reserved};
    report.update.kind = model::OrderUpdateKind::kAccepted;
    auto& order = report.update.order;
    order.client_order_id = request.order_id;
    order.instrument_id = request.request.instrument_id;
    order.side = request.request.side;
    order.limit_price = request.request.limit_price;
    order.original_qty = request.request.quantity;
    order.leaves_qty = request.request.quantity;
    order.state = model::OrderState::kWorking;
    order.report_sequence = 1;
    if (order.broker_order_id.Assign("timer-order") != base::ErrorCode::kOk ||
        !Send(report))
      return;
    Message cancel;
    if (!ReceiveBusiness(cancel) || cancel.type != MessageType::kCancelOrder ||
        cancel.order_id != order.client_order_id ||
        cancel.broker_order_id != order.broker_order_id)
      return;
    ++cancels;
    report.account.revision = 3;
    report.account.cash = {kInitialCash, 0, kInitialCash};
    report.update.kind = model::OrderUpdateKind::kCanceled;
    report.update.cancel_request_id = cancel.cancel_id;
    order.state = model::OrderState::kCanceled;
    order.leaves_qty = 0;
    order.cancel_request_id = cancel.cancel_id;
    order.report_sequence = 2;
    if (!Send(report)) return;
    canceled_.store(true);
    Message stop;
    if (!ReceiveBusiness(stop) || stop.type != MessageType::kStop) return;
    Message drained;
    drained.type = MessageType::kDrained;
    completed = Send(drained);
  }

  int orders = 0;
  int cancels = 0;
  bool completed = false;

 private:
  bool Active() const noexcept {
    return !aborted_.load() && std::chrono::steady_clock::now() < deadline_;
  }
  bool Receive(Message& message) {
    while (Active()) {
      const auto result = connection_.Poll(message);
      if (result.state == trading::ReceiveState::kMessage) return true;
      if (result.state != trading::ReceiveState::kIdle) return false;
      std::this_thread::yield();
    }
    return false;
  }
  // Heartbeats may interleave with business messages without changing their
  // ordering or the server facts synthesized by this peer.
  bool ReceiveBusiness(Message& message) {
    while (Receive(message)) {
      if (message.type != MessageType::kHeartbeat) return true;
    }
    return false;
  }
  bool Send(Message message) {
    message.generation = kGeneration;
    if (!connection_.Queue(message).ok()) return false;
    while (!connection_.empty() && Active()) {
      if (!connection_.Service().ok()) return false;
      std::this_thread::yield();
    }
    return connection_.empty();
  }

  trading::Listener listener_;
  trading::Connection connection_;
  std::atomic<bool> aborted_ = false;
  std::atomic<bool> canceled_ = false;
  std::chrono::steady_clock::time_point deadline_;
};

TEST(RemoteTimerRunnerTest, TimerSubmitsCancelsAndDrainsThroughTcpRuntime) {
  TimerPeer peer;
  ASSERT_TRUE(peer.Open());
  testing::ConfigFile trading(
      R"({"adapter":"tcp","address":"127.0.0.1","port":)" +
      std::to_string(peer.port()) +
      R"(,"client_id":19,"token":"timer-test","connect_timeout_ms":1000,)"
      R"("heartbeat_timeout_ms":1000,"shutdown_timeout_ms":1000})");
  ASSERT_TRUE(trading.valid());
  loop_testing::EventPipe events;
  ASSERT_TRUE(events.valid());
  RemoteTimerEnvironment environment(peer.canceled());
  base::SteadyMonotonicClock clock;
  StrategyAssembly assembly(clock);
  ASSERT_TRUE(
      assembly
          .Load({YQUANT_TEST_TIMER_TRADING_PATH, YQUANT_TEST_CONFIG_PATH,
                 YQUANT_TEST_MARKET_DATA_CONFIG_PATH, trading.path()},
                environment)
          .ok());
  IdleTestSource source;
  TestStrategyRunner runner(clock, environment, source, assembly.trading());
  std::thread worker([&peer] { peer.Serve(); });
  const auto loaded = runner.LoadAndStart(assembly.start_options());
  const auto result = loaded.ok() ? runner.Run() : loaded;
  peer.Abort();
  worker.join();
  ASSERT_TRUE(loaded.ok()) << runner.error_stage() << ": "
                           << runner.error_detail();
  ASSERT_TRUE(result.ok()) << runner.error_stage() << ": "
                           << runner.error_detail();
  EXPECT_TRUE(peer.completed);
  EXPECT_EQ(peer.orders, 1);
  EXPECT_EQ(peer.cancels, 1);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(events.Read(), "CSHNAHKXcTD");
}

}  // namespace
}  // namespace yquant::strategyd
