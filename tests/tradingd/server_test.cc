#include "tradingd/server.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "scoped_working_directory.h"
#include "strategyd/market_data_config.h"
#include "tradingd/connection.h"

namespace yquant::tradingd {
namespace {

class TestClock final : public strategyd::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 1000000;
};

ServerConfig Config() {
  ServerConfig config;
  config.listen.port = 0;
  std::snprintf(config.token, sizeof(config.token), "test-token");
  config.market_data.source_id = {1};
  config.market_data.interval_ns = 1000000;
  config.market_data.instrument_count = 1;
  auto& item = config.market_data.instruments[0];
  EXPECT_EQ(model::MakeInstrumentId("SSE", "600000", item.instrument.id),
            base::ErrorCode::kOk);
  item.instrument.venue = model::Venue::kSse;
  std::memcpy(item.instrument.symbol, "600000", model::kSymbolLength);
  item.instrument.price_decimals = 2;
  item.bid_price = 1000;
  item.ask_price = 1001;
  item.bid_qty = 100;
  item.ask_qty = 100;
  item.price_step = 1;
  config.trading.mode = strategyd::MockTradingMode::kWorking;
  config.trading.initial_account.cash_balance = 1000000000;
  return config;
}

struct Peer {
  Connection connection;
  std::uint64_t generation = 0;
  std::vector<Message> received;
};

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(server.Init(config).ok()); }

  void Restart() {
    first.connection.Close();
    second.connection.Close();
    first.received.clear();
    second.received.clear();
    ASSERT_TRUE(server.Init(config).ok()) << server.error_detail();
  }

  void Connect(Peer& peer, std::uint64_t id = 1,
               const char* token = "test-token") {
    Endpoint endpoint;
    endpoint.port = server.port();
    ASSERT_TRUE(peer.connection.Connect(endpoint, 1000000000).ok());
    Message hello;
    hello.type = MessageType::kHello;
    hello.client_id = id;
    std::snprintf(hello.token, sizeof(hello.token), "%s", token);
    hello.instrument_count = 1;
    hello.instruments[0] = {config.market_data.instruments[0].instrument.id, 2};
    ASSERT_TRUE(peer.connection.Queue(hello).ok());
    Pump();
    for (const auto& message : peer.received)
      if (message.type == MessageType::kWelcome)
        peer.generation = message.generation;
  }

  void Pump() {
    for (int iteration = 0; iteration < 40; ++iteration) {
      for (auto* peer : {&first, &second}) {
        if (peer->connection.open()) (void)peer->connection.Service();
      }
      ASSERT_TRUE(server.Service(clock.now).ok()) << server.error_detail();
      for (auto* peer : {&first, &second}) {
        for (int count = 0; count < 64 && peer->connection.open(); ++count) {
          Message message;
          const auto result = peer->connection.Poll(message);
          if (result.state == ReceiveState::kMessage)
            peer->received.push_back(message);
          else
            break;
        }
      }
      std::this_thread::yield();
    }
  }

  void Advance() {
    clock.now += 1000000;
    Pump();
  }

  void Order(Peer& peer, std::uint64_t id, std::int64_t qty,
             model::OrderSide side = model::OrderSide::kBuy) {
    Message message;
    message.type = MessageType::kNewOrder;
    message.generation = peer.generation;
    message.order_id = {id};
    message.request = {config.market_data.instruments[0].instrument.id, side,
                       1000, qty};
    ASSERT_TRUE(peer.connection.Queue(message).ok());
  }

  const Message* Update(const Peer& peer, model::OrderUpdateKind kind,
                        std::uint64_t order_id = 1) const {
    for (auto it = peer.received.rbegin(); it != peer.received.rend(); ++it)
      if (it->type == MessageType::kOrderUpdate && it->update.kind == kind &&
          it->update.order.client_order_id.value == order_id)
        return &*it;
    return nullptr;
  }

  bool Has(const Peer& peer, MessageType type) const {
    return std::any_of(
        peer.received.begin(), peer.received.end(),
        [type](const Message& message) { return message.type == type; });
  }

  TestClock clock;
  ServerConfig config = Config();
  Server server{clock};
  Peer first;
  Peer second;
};

TEST_F(ServerTest, HandshakeReturnsAuthoritativeAccountAndSubset) {
  Connect(first);
  ASSERT_NE(first.generation, 0U);
  ASSERT_EQ(first.received.front().type, MessageType::kWelcome);
  const auto& welcome = first.received.front();
  EXPECT_EQ(welcome.instrument_count, 1U);
  EXPECT_EQ(welcome.account.revision, 1U);
  EXPECT_EQ(welcome.account.cash.balance, 1000000000);
  EXPECT_EQ(welcome.account.cash.available, 1000000000);
}

TEST_F(ServerTest, WrongTokenDoesNotAuthenticate) {
  Connect(first, 1, "incorrect-token");
  EXPECT_FALSE(Has(first, MessageType::kWelcome));
  EXPECT_EQ(first.generation, 0U);
}

TEST_F(ServerTest, MultipleClientsCannotReserveTheSameCash) {
  Connect(first, 11);
  Connect(second, 22);
  ASSERT_NE(first.generation, 0U);
  ASSERT_EQ(first.generation, second.generation);
  Order(first, 1, 60);
  Order(second, 1, 60);
  Pump();
  EXPECT_EQ(server.account().cash.reserved, 600000000);
  EXPECT_EQ(server.account().cash.available, 400000000);
  ASSERT_NE(Update(second, model::OrderUpdateKind::kRejected), nullptr);
  EXPECT_EQ(Update(second, model::OrderUpdateKind::kRejected)
                ->update.order.report_sequence,
            1U);
  Advance();
  ASSERT_NE(Update(first, model::OrderUpdateKind::kAccepted), nullptr);
  EXPECT_EQ(
      Update(first, model::OrderUpdateKind::kAccepted)->account.cash.reserved,
      600000000);
}

TEST_F(ServerTest, MultipleClientsCannotReserveTheSameShares) {
  config.trading.initial_account.position_count = 1;
  config.trading.initial_account.positions[0] = {
      config.market_data.instruments[0].instrument.id, 100, 100, 0};
  Restart();
  Connect(first, 11);
  Connect(second, 22);
  Order(first, 1, 60, model::OrderSide::kSell);
  Order(second, 1, 60, model::OrderSide::kSell);
  Pump();
  ASSERT_EQ(server.account().position_count, 1U);
  EXPECT_EQ(server.account().positions[0].available_qty, 40);
  EXPECT_EQ(server.account().positions[0].reserved_qty, 60);
  EXPECT_NE(Update(second, model::OrderUpdateKind::kRejected), nullptr);
}

TEST_F(ServerTest, IdenticalLocalOrderIdsRouteFillsOnlyToTheirOwners) {
  config.trading.mode = strategyd::MockTradingMode::kFill;
  Restart();
  Connect(first, 11);
  Connect(second, 22);
  Order(first, 1, 10);
  Order(second, 1, 20);
  Pump();
  Advance();
  Advance();
  Advance();
  const auto* first_fill = Update(first, model::OrderUpdateKind::kFill);
  const auto* second_fill = Update(second, model::OrderUpdateKind::kFill);
  ASSERT_NE(first_fill, nullptr);
  ASSERT_NE(second_fill, nullptr);
  EXPECT_EQ(first_fill->fill.quantity, 5);
  EXPECT_EQ(second_fill->fill.quantity, 10);
  EXPECT_EQ(first_fill->update.order.cumulative_filled_qty, 10);
  EXPECT_EQ(second_fill->update.order.cumulative_filled_qty, 20);
  EXPECT_EQ(first_fill->fill.client_order_id.value, 1U);
  EXPECT_EQ(second_fill->fill.client_order_id.value, 1U);
  EXPECT_NE(first_fill->fill.broker_order_id,
            second_fill->fill.broker_order_id);
  EXPECT_EQ(server.account().cash.balance, 700000000);
}

TEST_F(ServerTest, DisconnectCancelsOnlyTheOwnersOrders) {
  Connect(first, 11);
  Connect(second, 22);
  Order(first, 1, 60);
  Order(second, 1, 20);
  Pump();
  Advance();
  EXPECT_EQ(server.account().cash.reserved, 800000000);
  first.connection.Close();
  Pump();
  Advance();
  Advance();
  EXPECT_EQ(server.account().cash.reserved, 200000000);
  EXPECT_EQ(server.account().cash.available, 800000000);
  EXPECT_EQ(Update(second, model::OrderUpdateKind::kCanceled), nullptr);
  Order(second, 2, 10);
  Pump();
  EXPECT_EQ(server.account().cash.reserved, 300000000);
}

TEST_F(ServerTest, StopDrainsOneClientWithoutStoppingOtherClients) {
  Connect(first, 11);
  Connect(second, 22);
  Order(first, 1, 60);
  Order(second, 1, 20);
  Pump();
  Advance();
  Message stop;
  stop.type = MessageType::kStop;
  stop.generation = first.generation;
  ASSERT_TRUE(first.connection.Queue(stop).ok());
  Pump();
  EXPECT_FALSE(Has(first, MessageType::kDrained));
  Advance();
  Advance();
  ASSERT_TRUE(Has(first, MessageType::kDrained));
  const auto* canceled = Update(first, model::OrderUpdateKind::kCanceled);
  ASSERT_NE(canceled, nullptr);
  EXPECT_EQ(canceled->update.cancel_request_id.value, 0U);
  EXPECT_EQ(server.account().cash.reserved, 200000000);
  Order(second, 2, 10);
  Pump();
  EXPECT_EQ(server.account().cash.reserved, 300000000);
}

TEST_F(ServerTest, DisconnectIdentityCannotBeReusedWithinGeneration) {
  Connect(first, 11);
  ASSERT_NE(first.generation, 0U);
  first.connection.Close();
  Pump();
  Connect(second, 11);
  EXPECT_FALSE(Has(second, MessageType::kWelcome));
}

TEST_F(ServerTest, DrainedClientReceivesNoFurtherAccountBroadcasts) {
  config.trading.mode = strategyd::MockTradingMode::kFill;
  Restart();
  Connect(first, 11);
  Connect(second, 22);
  Message stop;
  stop.type = MessageType::kStop;
  stop.generation = first.generation;
  ASSERT_TRUE(first.connection.Queue(stop).ok());
  Pump();
  ASSERT_TRUE(Has(first, MessageType::kDrained));
  first.received.clear();
  Order(second, 1, 10);
  Pump();
  Advance();
  Advance();
  Advance();
  EXPECT_NE(Update(second, model::OrderUpdateKind::kFill), nullptr);
  EXPECT_FALSE(Has(first, MessageType::kAccount));
  EXPECT_FALSE(Has(first, MessageType::kOrderUpdate));
}

TEST_F(ServerTest, IncorrectGenerationDisconnectsAndCancelsTheClient) {
  Connect(first, 11);
  Order(first, 1, 60);
  Pump();
  Advance();
  Message wrong;
  wrong.type = MessageType::kHeartbeat;
  wrong.generation = first.generation ^ 1;
  ASSERT_TRUE(first.connection.Queue(wrong).ok());
  Pump();
  Advance();
  Advance();
  EXPECT_EQ(server.account().cash.reserved, 0);
  EXPECT_EQ(server.account().cash.available, 1000000000);
}

TEST_F(ServerTest,
       CancelFilledOrderReportsRejectionWithoutInventingCancellation) {
  config.trading.mode = strategyd::MockTradingMode::kFill;
  Restart();
  Connect(first, 11);
  Order(first, 1, 10);
  Pump();
  Advance();
  Advance();
  Advance();
  ASSERT_NE(Update(first, model::OrderUpdateKind::kFill), nullptr);
  Message cancel;
  cancel.type = MessageType::kCancelOrder;
  cancel.generation = first.generation;
  cancel.order_id = {1};
  cancel.cancel_id = {1};
  ASSERT_TRUE(first.connection.Queue(cancel).ok());
  Pump();
  const auto* reply = Update(first, model::OrderUpdateKind::kCancelRejected);
  ASSERT_NE(reply, nullptr);
  EXPECT_EQ(reply->update.order.state, model::OrderState::kFilled);
  EXPECT_EQ(reply->update.cancel_request_id.value, 1U);
  EXPECT_EQ(Update(first, model::OrderUpdateKind::kCanceled), nullptr);
  EXPECT_EQ(server.account().cash.balance, 900000000);
}

TEST_F(ServerTest, ShutdownWaitsForConfirmedCancellations) {
  Connect(first, 11);
  Order(first, 1, 60);
  Pump();
  Advance();
  server.BeginStopping(clock.now);
  Pump();
  EXPECT_FALSE(server.is_drained());
  EXPECT_EQ(server.account().cash.reserved, 600000000);
  Advance();
  Advance();
  EXPECT_TRUE(server.is_drained());
  EXPECT_FALSE(Has(first, MessageType::kDrained));
  EXPECT_EQ(server.account().cash.reserved, 0);
}

TEST_F(ServerTest, AccountRevisionsDoNotRegressAndOrdersUseClientSequences) {
  config.trading.mode = strategyd::MockTradingMode::kFill;
  Restart();
  Connect(first, 11);
  Order(first, 1, 10);
  Pump();
  Advance();
  Advance();
  Advance();
  std::uint64_t revision = 0;
  std::uint64_t report = 0;
  for (const auto& message : first.received) {
    if (message.type == MessageType::kWelcome ||
        message.type == MessageType::kAccount ||
        message.type == MessageType::kOrderUpdate) {
      EXPECT_GE(message.account.revision, revision);
      revision = message.account.revision;
    }
    if (message.type == MessageType::kOrderUpdate)
      EXPECT_EQ(message.update.order.report_sequence, ++report);
  }
  EXPECT_GT(revision, 1U);
  EXPECT_GE(report, 2U);
}

TEST_F(ServerTest, MissingHeartbeatDisconnectsAndCancelsWorkingOrders) {
  Connect(first, 11);
  Order(first, 1, 60);
  Pump();
  Advance();
  clock.now += config.heartbeat_timeout_ns;
  Pump();
  Advance();
  Advance();
  EXPECT_EQ(server.account().cash.reserved, 0);
}

TEST(TradingdConfigTest,
     TypedConfigurationRejectsInvalidLimitsAndRemoteBroker) {
  auto config = Config();
  base::ConfigError error;
  EXPECT_TRUE(ValidateConfig(config, error).ok());
  config.max_clients = 17;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "max_clients");
  config = Config();
  config.token[0] = '\0';
  EXPECT_FALSE(ValidateConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "token");
  config = Config();
  config.heartbeat_timeout_ns = 999999999;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
  config = Config();
  config.trading.adapter = strategyd::TradingAdapterKind::kTcp;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
}

TEST(TradingdConfigTest, UnknownBootstrapInstrumentIsRejected) {
  auto config = Config();
  config.trading.initial_account.position_count = 1;
  auto& position = config.trading.initial_account.positions[0];
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600001", position.instrument_id),
            base::ErrorCode::kOk);
  position.total_qty = 100;
  position.available_qty = 100;
  base::ConfigError error;
  EXPECT_FALSE(ValidateConfig(config, error).ok());
  EXPECT_STREQ(error.field_path, "trading_config.initial_positions");
}

// Isolates referenced files; each configuration test selects its own cwd.
class ConfigFiles {
 public:
  ConfigFiles() { valid_ = mkdtemp(directory_) != nullptr; }
  ~ConfigFiles() {
    std::error_code error;
    if (valid_) std::filesystem::remove_all(directory_, error);
  }
  bool valid() const { return valid_; }
  std::string Path(const char* name) const {
    return std::string(directory_) + "/" + name;
  }
  bool CopyCatalog() const {
    std::error_code error;
    return std::filesystem::copy_file(YQUANT_TEST_INSTRUMENT_CATALOG_PATH,
                                      Path("instruments.json"), error);
  }
  bool Write(const char* name, std::string_view value) const {
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(Path(name)).parent_path(), error);
    if (error) return false;
    auto* file = std::fopen(Path(name).c_str(), "wb");
    if (file == nullptr) return false;
    const bool written =
        std::fwrite(value.data(), 1, value.size(), file) == value.size();
    return std::fclose(file) == 0 && written;
  }

 private:
  char directory_[64] = "/tmp/yquant-tradingd-config-XXXXXX";
  bool valid_ = false;
};

TEST(TradingdConfigTest, LoadsStrictConfigurationAndRelativeReferences) {
  ConfigFiles files;
  ASSERT_TRUE(files.valid());
  ASSERT_TRUE(files.CopyCatalog());
  ::yquant::testing::ScopedWorkingDirectory working_directory(
      files.Path("").c_str());
  ASSERT_TRUE(working_directory.valid());
  ASSERT_TRUE(files.Write("market.json", R"({"source_id":1,"adapter":"mock",
    "instrument_catalog":"instruments.json",
    "interval_ms":1,"instruments":[{"venue":"SSE","symbol":"600000",
    "bid_price":1000,"bid_qty":100,"ask_price":1001,
    "ask_qty":100,"price_step":1}]})"));
  ASSERT_TRUE(files.Write("trading.json", R"({"adapter":"mock","mode":"working",
    "report_interval_ms":1,"shutdown_timeout_ms":5000,"initial_cash":1234,
    "initial_positions":[]})"));
  constexpr auto kValid = R"({"listen":{"address":"127.0.0.1","port":19200},
    "market_data_config":"market.json","trading_config":"trading.json",
    "token":"test-token","max_clients":3,"heartbeat_timeout_ms":1500,
    "shutdown_timeout_ms":2000})";
  ASSERT_TRUE(files.Write("gateway/server.json", kValid));
  ConfigLoader loader;
  ASSERT_TRUE(loader.Load(files.Path("gateway/server.json").c_str()).ok())
      << loader.error().field_path << ": " << loader.error().message;
  EXPECT_EQ(loader.config().max_clients, 3U);
  EXPECT_EQ(loader.config().heartbeat_timeout_ns, 1500000000);
  EXPECT_EQ(loader.config().shutdown_timeout_ns, 2000000000);
  EXPECT_EQ(loader.config().trading.initial_account.cash_balance, 1234);
  EXPECT_EQ(loader.config().market_data.instrument_count, 1U);
  EXPECT_EQ(
      loader.config().market_data.instruments[0].instrument.price_decimals, 2);

  ASSERT_TRUE(
      files.Write("gateway/server.json", R"({"listen":{"address":"127.0.0.1",
    "port":19200},"market_data_config":"market.json",
    "trading_config":"trading.json","token":"test-token","unexpected":1})"));
  EXPECT_FALSE(loader.Load(files.Path("gateway/server.json").c_str()).ok());
  EXPECT_STREQ(loader.error().field_path, "unexpected");
  EXPECT_EQ(loader.config().market_data.instrument_count, 0U);

  ASSERT_TRUE(
      files.Write("gateway/server.json", R"({"listen":{"address":"127.0.0.1",
    "port":19200},"market_data_config":"market.json",
    "trading_config":"trading.json","token":"a","token":"b"})"));
  EXPECT_FALSE(loader.Load(files.Path("gateway/server.json").c_str()).ok());
}

TEST(TradingdConfigTest, NestedMarketConfigAndClientShareCatalogDefinitions) {
  ConfigFiles files;
  ASSERT_TRUE(files.valid());
  ASSERT_TRUE(files.CopyCatalog());
  ::yquant::testing::ScopedWorkingDirectory working_directory(
      files.Path("").c_str());
  ASSERT_TRUE(working_directory.valid());
  ASSERT_TRUE(files.Write("sources/market.json",
                          R"({"source_id":1,"adapter":"mock",
    "instrument_catalog":"instruments.json","interval_ms":1,
    "instruments":[{"venue":"SZSE","symbol":"000557","bid_price":43000,
    "bid_qty":100,"ask_price":43001,"ask_qty":100,"price_step":1}]})"));
  ASSERT_TRUE(files.Write("broker/trading.json",
                          R"({"adapter":"mock","mode":"working",
    "report_interval_ms":1,"shutdown_timeout_ms":5000,"initial_cash":1234,
    "initial_positions":[]})"));
  ASSERT_TRUE(files.Write("gateway/server.json",
                          R"({"listen":{"address":"127.0.0.1","port":19200},
    "market_data_config":"sources/market.json",
    "trading_config":"broker/trading.json","token":"test-token"})"));
  ASSERT_TRUE(files.Write("clients/market.json",
                          R"({"source_id":1,"adapter":"shm",
    "instrument_catalog":"instruments.json","runtime_directory":")" +
                              files.Path("stream") +
                              R"(","stream":"quotes",
    "instruments":[{"venue":"SZSE","symbol":"000557"}]})"));

  ConfigLoader gateway;
  ASSERT_TRUE(gateway.Load(files.Path("gateway/server.json").c_str()).ok())
      << gateway.error().field_path << ": " << gateway.error().message;
  strategyd::MarketDataConfigLoader client;
  ASSERT_TRUE(client.Load(files.Path("clients/market.json").c_str()).ok())
      << client.error().field_path << ": " << client.error().message;
  ASSERT_EQ(gateway.config().market_data.instrument_count, 1U);
  ASSERT_EQ(client.config().instrument_count, 1U);
  const auto& definition =
      gateway.config().market_data.instruments[0].instrument;
  EXPECT_EQ(definition.price_decimals, 4);
  EXPECT_EQ(definition, client.config().instruments[0].instrument);
}

}  // namespace
}  // namespace yquant::tradingd
