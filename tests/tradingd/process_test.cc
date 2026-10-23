#include <arpa/inet.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>

#include "gtest/gtest.h"
#include "marketd/test_support.h"
#include "process_support.h"
#include "scoped_working_directory.h"
#include "yquant/base/format.h"
#include "yquant/trading/connection.h"

namespace yquant::tradingd {
namespace {
using marketd::testing::RuntimeDirectory;
using testing::ChildProcess;

uint16_t AvailablePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof(address);
  const bool ok =
      ::bind(fd, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
      ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0;
  ::close(fd);
  return ok ? ntohs(address.sin_port) : 0;
}

// Uses real daemons over loopback TCP; no mock calls replace the wire path.
class TradingdProcessTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.valid());
    ASSERT_TRUE(working_directory_.valid());
    port_ = AvailablePort();
    ASSERT_NE(port_, 0);
    ASSERT_TRUE(directory_.Write(
        "mock.json",
        R"({"adapter":"mock","source_id":1,"interval_ms":10,)"
        R"("instrument_catalog":"instruments.json","instruments":[)"
        R"({"venue":"SSE","symbol":"600000",)"
        R"("bid_price":1000,"bid_qty":100,"ask_price":1001,"ask_qty":200,"price_step":0}]})"));
    ASSERT_TRUE(
        directory_.Write("market.json", R"({"adapter":"shm","source_id":1,
"instrument_catalog":"instruments.json","runtime_directory":"/tmp/yquant-test-feed",
"stream":"quotes","poll_interval_us":1000,
"instruments":[{"venue":"SSE","symbol":"600000"}]})"));
    ASSERT_TRUE(directory_.Write(
        "strategy.json",
        R"({"strategy_id":"shared-account","parameters":{)"
        R"("venue":"SSE","symbol":"600000","side":"buy","quantity":200}})"));
    ASSERT_TRUE(directory_.Write(
        "server.json",
        R"({"listen":{"address":"127.0.0.1","port":)" + std::to_string(port_) +
            R"(},"market_data_config":"market.json","trading_config":"broker.json",)"
            R"("token":"test-secret","heartbeat_timeout_ms":3000,"shutdown_timeout_ms":2000})"));
    for (unsigned id = 1; id <= 3; ++id) {
      ASSERT_TRUE(directory_.Write(
          "client" + std::to_string(id) + ".json",
          R"({"adapter":"tcp","address":"127.0.0.1","port":)" +
              std::to_string(port_) + R"(,"client_id":)" + std::to_string(id) +
              R"(,"token":"test-secret","connect_timeout_ms":2000,)"
              R"("heartbeat_timeout_ms":3000,"shutdown_timeout_ms":2000})"));
    }
  }
  void Broker(std::string_view mode, int64_t cash) {
    ASSERT_TRUE(directory_.Write(
        "broker.json",
        R"({"adapter":"mock","mode":")" + std::string(mode) +
            R"(","report_interval_ms":1,"shutdown_timeout_ms":2000,"initial_cash":)" +
            std::to_string(cash) +
            R"(,"initial_positions":[{"venue":"SSE",)"
            R"("symbol":"600000","total_qty":1000,"available_qty":800}]})"));
  }
  int StartServer(ChildProcess& process) {
    return process.Start(YQUANT_TEST_TRADINGD_PATH,
                         {"--config", directory_.File("server.json")});
  }
  int StartStrategy(ChildProcess& process, unsigned id,
                    const char* market_config = "market.json") {
    return process.Start(
        YQUANT_TEST_STRATEGYD_PATH,
        {"--strategy", YQUANT_TEST_SHARED_STRATEGY_PATH, "--config",
         directory_.File("strategy.json"), "--market-data-config",
         directory_.File(market_config), "--trading-config",
         directory_.File("client" + std::to_string(id) + ".json")});
  }
  void Stop(ChildProcess& process) {
    ASSERT_TRUE(process.Signal(SIGTERM)) << process.output();
    ASSERT_TRUE(process.Wait()) << process.output();
    EXPECT_EQ(process.exit_code(), 0) << process.output();
  }
  // Opens an observer as another authenticated strategy identity. It receives
  // the same account view, but submits no orders or cancellations.
  void Observe(trading::Connection& connection, trading::Message& welcome) {
    trading::Endpoint endpoint;
    endpoint.port = port_;
    ASSERT_TRUE(connection.Connect(endpoint, 2000000000ULL).ok())
        << connection.error_detail();
    trading::Message hello;
    hello.type = trading::MessageType::kHello;
    hello.client_id = 99;
    ::yquant::base::FormatTo(hello.token, sizeof(hello.token), "test-secret");
    hello.instrument_count = 1;
    ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", hello.instruments[0].id),
              base::ErrorCode::kOk);
    hello.instruments[0].price_decimals = 2;
    ASSERT_TRUE(connection.Queue(hello).ok());
    ASSERT_TRUE(Read(connection, welcome));
    ASSERT_EQ(welcome.type, trading::MessageType::kWelcome);
  }
  bool Read(trading::Connection& connection, trading::Message& message) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto result = connection.Poll(message);
      if (result.state == trading::ReceiveState::kMessage) {
        return true;
      }
      if (result.state != trading::ReceiveState::kIdle) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
  RuntimeDirectory directory_;
  ::yquant::testing::ScopedWorkingDirectory working_directory_{
      directory_.path()};
  uint16_t port_ = 0;
};

TEST_F(TradingdProcessTest, TwoStrategiesFillAgainstOneAccount) {
  constexpr int64_t kCash = 100000000000000;
  Broker("fill", kCash);
  ChildProcess server;
  ASSERT_EQ(StartServer(server), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ASSERT_TRUE(directory_.Write(
      "marketd.json",
      R"({"source":{"adapter":"mock","config":"mock.json"},)"
      R"("publishers":[{"adapter":"shm","runtime_directory":")" +
          std::string(directory_.path()) +
          R"(","stream":"quotes","capacity":16384,"max_payload_size":80}]})"));
  ASSERT_TRUE(directory_.Write(
      "shared_market.json",
      R"({"adapter":"shm","source_id":1,)"
      R"("instrument_catalog":"instruments.json","runtime_directory":")" +
          std::string(directory_.path()) +
          R"(","stream":"quotes","poll_interval_us":0,"instruments":[)"
          R"({"venue":"SSE","symbol":"600000"}]})"));
  ChildProcess marketd;
  ASSERT_EQ(marketd.Start(YQUANT_TEST_MARKETD_PATH,
                          {"--config", directory_.File("marketd.json")}),
            0);
  ASSERT_TRUE(marketd.WaitForOutput("Running")) << marketd.output();
  ChildProcess first;
  ChildProcess second;
  ASSERT_EQ(StartStrategy(first, 1, "shared_market.json"), 0);
  ASSERT_EQ(StartStrategy(second, 2, "shared_market.json"), 0);
  ASSERT_TRUE(first.WaitForOutput("order state=filled cumulative=200"))
      << first.output();
  ASSERT_TRUE(second.WaitForOutput("order state=filled cumulative=200"))
      << second.output();
  trading::Connection observer;
  trading::Message welcome;
  Observe(observer, welcome);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  EXPECT_EQ(welcome.account.cash.balance, kCash - 4004000000LL);
  EXPECT_EQ(welcome.account.cash.reserved, 0);
  ASSERT_EQ(welcome.account.position_count, 1U);
  EXPECT_EQ(welcome.account.positions[0].total_qty, 1400);
  EXPECT_EQ(welcome.account.positions[0].available_qty, 800);
  Stop(first);
  Stop(second);
  EXPECT_NE(first.output().find("stopped state=filled filled_qty=200"),
            std::string::npos);
  EXPECT_NE(second.output().find("stopped state=filled filled_qty=200"),
            std::string::npos);
  observer.Close();
  Stop(marketd);
  Stop(server);
}

TEST_F(TradingdProcessTest,
       SharedCashIsReservedOnceAndDisconnectReleasesAfterCancel) {
  constexpr int64_t kCash = 2002000000;
  Broker("working", kCash);
  ChildProcess server;
  ASSERT_EQ(StartServer(server), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ChildProcess first;
  ChildProcess second;
  ASSERT_EQ(StartStrategy(first, 1), 0);
  ASSERT_TRUE(first.WaitForOutput("order state=working")) << first.output();
  ASSERT_EQ(StartStrategy(second, 2), 0);
  ASSERT_TRUE(second.WaitForOutput("order state=rejected")) << second.output();
  ASSERT_TRUE(first.Signal(SIGKILL));
  ASSERT_TRUE(first.Wait());
  trading::Connection observer;
  trading::Message message;
  Observe(observer, message);
  ASSERT_FALSE(::testing::Test::HasFatalFailure());
  for (unsigned count = 0;
       message.account.cash.available != kCash && count < 20; ++count) {
    ASSERT_TRUE(Read(observer, message)) << observer.error_detail();
  }
  ASSERT_EQ(message.account.cash.available, kCash);
  EXPECT_EQ(message.account.cash.reserved, 0);
  ChildProcess third;
  ASSERT_EQ(StartStrategy(third, 3), 0);
  ASSERT_TRUE(third.WaitForOutput("order state=working")) << third.output();
  Stop(third);
  EXPECT_NE(third.output().find("stopped state=canceled"), std::string::npos);
  Stop(second);
  observer.Close();
  Stop(server);
}

TEST_F(TradingdProcessTest,
       CheckConfigDoesNotBindAndExistingIdentityCannotReconnect) {
  Broker("working", 100000000000000);
  ChildProcess server;
  ASSERT_EQ(StartServer(server), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ChildProcess check;
  ASSERT_EQ(check.Start(
                YQUANT_TEST_TRADINGD_PATH,
                {"--config", directory_.File("server.json"), "--check-config"}),
            0);
  ASSERT_TRUE(check.Wait()) << check.output();
  EXPECT_EQ(check.exit_code(), 0) << check.output();
  ChildProcess first;
  ASSERT_EQ(StartStrategy(first, 1), 0);
  ASSERT_TRUE(first.WaitForOutput("order state=working")) << first.output();
  Stop(first);
  ChildProcess duplicate;
  ASSERT_EQ(StartStrategy(duplicate, 1), 0);
  ASSERT_TRUE(duplicate.Wait()) << duplicate.output();
  EXPECT_NE(duplicate.exit_code(), 0) << duplicate.output();
  EXPECT_EQ(duplicate.output().find("shared_account: ready"),
            std::string::npos);
  Stop(server);
}

TEST_F(TradingdProcessTest, GatewayLossStopsStrategyWithoutReconnecting) {
  Broker("working", 100000000000000);
  ChildProcess server;
  ASSERT_EQ(StartServer(server), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ChildProcess strategy;
  ASSERT_EQ(StartStrategy(strategy, 1), 0);
  ASSERT_TRUE(strategy.WaitForOutput("order state=working"))
      << strategy.output();
  ASSERT_TRUE(server.Signal(SIGKILL));
  ASSERT_TRUE(server.Wait());
  ASSERT_TRUE(strategy.Wait()) << strategy.output();
  EXPECT_NE(strategy.exit_code(), 0) << strategy.output();
  EXPECT_NE(strategy.output().find("unconfirmed_order"), std::string::npos);
  EXPECT_NE(strategy.output().find("stopped state=working"), std::string::npos);
}

TEST_F(TradingdProcessTest, GatewayShutdownCancelsBeforeClosingClientStream) {
  Broker("working", 100000000000000);
  ChildProcess server;
  ASSERT_EQ(StartServer(server), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ChildProcess strategy;
  ASSERT_EQ(StartStrategy(strategy, 1), 0);
  ASSERT_TRUE(strategy.WaitForOutput("order state=working"))
      << strategy.output();
  Stop(server);
  ASSERT_TRUE(strategy.Wait()) << strategy.output();
  EXPECT_NE(strategy.exit_code(), 0);
  EXPECT_NE(strategy.output().find("order state=canceled"), std::string::npos)
      << strategy.output();
  EXPECT_NE(strategy.output().find("stopped state=canceled"), std::string::npos)
      << strategy.output();
  EXPECT_EQ(strategy.output().find("unconfirmed_order="), std::string::npos)
      << strategy.output();
}

}  // namespace
}  // namespace yquant::tradingd
