#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "file_test_support.h"
#include "gtest/gtest.h"
#include "md/pipeline_test_support.h"
#include "process_support.h"
#include "scoped_working_directory.h"
#include "tradingd/server.h"
#include "yquant/base/format.h"
#include "yquant/trading/connection.h"

namespace yquant::tradingd {
namespace {
using md::testing::RuntimeDirectory;

class PersistenceClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000000;
};

struct PersistentPeer {
  trading::Connection connection;
  uint64_t generation = 0;
  std::vector<trading::Message> messages;
};

// Drives the real TCP framing and shared ledger while keeping broker deadlines
// deterministic. A fresh Server object reloads state on every Restart.
class TradingdPersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory.valid());
    config.listen.port = 0;
    ::yquant::base::FormatTo(config.token, sizeof(config.token),
                             "persistence-token");
    config.market_data = md::testing::ScopeConfig();
    config.trading.mode = trading::MockTradingMode::kWorking;
    config.trading.initial_account.cash_balance = 1000000000;
    ::yquant::base::FormatTo(config.persistence.directory,
                             sizeof(config.persistence.directory), "{}/state",
                             directory.path());
    ::yquant::base::FormatTo(config.persistence.account_key,
                             sizeof(config.persistence.account_key),
                             "simulation/account-A/CNY");
    config.persistence.wal_segment_bytes = 4 * 1024 * 1024;
    config.persistence.initialize = true;
  }
  void Start() {
    ASSERT_TRUE(server.Init(config).ok()) << server.error_detail();
  }
  void Restart() {
    peer.connection.Close();
    peer.messages.clear();
    peer.generation = 0;
    server.Close();
    config.persistence.initialize = false;
    clock.now += 1000000;
    ASSERT_TRUE(server.Init(config).ok()) << server.error_detail();
  }
  void Connect(uint64_t id) {
    peer.connection.Close();
    peer.messages.clear();
    peer.generation = 0;
    trading::Endpoint endpoint;
    endpoint.port = server.port();
    ASSERT_TRUE(peer.connection.Connect(endpoint, 1000000000).ok());
    trading::Message hello;
    hello.type = trading::MessageType::kHello;
    hello.client_id = id;
    ::yquant::base::FormatTo(hello.token, sizeof(hello.token),
                             "persistence-token");
    hello.instrument_count = 1;
    hello.instruments[0] = {config.market_data.instruments[0].instrument.id, 2};
    ASSERT_TRUE(peer.connection.Queue(hello).ok());
    Pump();
    for (const auto& message : peer.messages) {
      if (message.type == trading::MessageType::kWelcome) {
        peer.generation = message.generation;
      }
    }
  }
  void Pump() {
    for (int iteration = 0; iteration < 40; ++iteration) {
      if (peer.connection.open()) {
        (void)peer.connection.Service();
      }
      ASSERT_TRUE(server.Service(clock.now).ok()) << server.error_detail();
      for (int count = 0; count < 64 && peer.connection.open(); ++count) {
        trading::Message message;
        const auto result = peer.connection.Poll(message);
        if (result.state != trading::ReceiveState::kMessage) {
          break;
        }
        peer.messages.push_back(message);
      }
      std::this_thread::yield();
    }
  }
  void Advance() {
    clock.now += 1000000;
    Pump();
  }
  void Order(uint64_t local_id = 1, int64_t quantity = 10) {
    trading::Message order;
    order.type = trading::MessageType::kNewOrder;
    order.generation = peer.generation;
    order.strategy_order_id = {local_id};
    order.request = {config.market_data.instruments[0].instrument.id,
                     model::OrderSide::kBuy, 1000, quantity};
    ASSERT_TRUE(peer.connection.Queue(order).ok());
    Pump();
  }
  const trading::Message* Update(model::OrderUpdateKind kind) const {
    for (auto it = peer.messages.rbegin(); it != peer.messages.rend(); ++it) {
      if (it->type == trading::MessageType::kOrderUpdate &&
          it->update.kind == kind) {
        return &*it;
      }
    }
    return nullptr;
  }
  std::string StateFile(const char* name) const {
    return std::string(config.persistence.directory) + "/" + name;
  }
  RuntimeDirectory directory;
  PersistenceClock clock;
  ServerConfig config;
  Server server{&clock};
  PersistentPeer peer;
};

TEST_F(TradingdPersistenceTest,
       FilledAccountSurvivesRestartWithoutBootstrapReset) {
  config.trading.mode = trading::MockTradingMode::kFill;
  Start();
  Connect(11);
  ASSERT_NE(peer.generation, 0U);
  Order();
  Advance();
  Advance();
  Advance();
  ASSERT_NE(Update(model::OrderUpdateKind::kFill), nullptr);
  ASSERT_EQ(Update(model::OrderUpdateKind::kFill)->update.order.state,
            model::OrderState::kFilled);
  EXPECT_EQ(server.account().cash.balance, 900000000);
  ASSERT_EQ(server.account().position_count, 1U);
  EXPECT_EQ(server.account().positions[0].total_qty, 10);
  config.trading.initial_account.cash_balance = 777;
  Restart();
  EXPECT_EQ(server.account().cash.balance, 900000000);
  EXPECT_EQ(server.account().cash.reserved, 0);
  ASSERT_EQ(server.account().position_count, 1U);
  EXPECT_EQ(server.account().positions[0].total_qty, 10);
  EXPECT_EQ(server.account().positions[0].available_qty, 0);
  Connect(22);
  ASSERT_NE(peer.generation, 0U);
  ASSERT_FALSE(peer.messages.empty());
  EXPECT_EQ(peer.messages.front().account.cash.balance, 900000000);
}

TEST_F(TradingdPersistenceTest,
       WorkingOrderIsCanceledAndNewOrderUsesFreshIdentity) {
  Start();
  Connect(11);
  const auto old_generation = peer.generation;
  Order();
  Advance();
  const auto* accepted = Update(model::OrderUpdateKind::kAccepted);
  ASSERT_NE(accepted, nullptr);
  const auto old_broker_id = accepted->update.order.adapter_order_id;
  EXPECT_EQ(server.account().cash.reserved, 100000000);
  Restart();
  EXPECT_EQ(server.account().cash.balance, 1000000000);
  EXPECT_EQ(server.account().cash.reserved, 0);
  Connect(22);
  ASSERT_NE(peer.generation, 0U);
  EXPECT_NE(peer.generation, old_generation);
  Order();
  Advance();
  accepted = Update(model::OrderUpdateKind::kAccepted);
  ASSERT_NE(accepted, nullptr);
  EXPECT_NE(accepted->update.order.adapter_order_id, old_broker_id);
  EXPECT_EQ(server.account().cash.reserved, 100000000);
  EXPECT_EQ(server.account().cash.balance, 1000000000);
}

TEST_F(TradingdPersistenceTest,
       RegisteredClientIdentityCannotResumeAfterRestart) {
  Start();
  Connect(11);
  ASSERT_NE(peer.generation, 0U);
  Restart();
  Connect(11);
  EXPECT_EQ(peer.generation, 0U);
  Connect(22);
  EXPECT_NE(peer.generation, 0U);
}

TEST_F(TradingdPersistenceTest, UniquePublisherLockExcludesSecondServer) {
  Start();
  Server second{&clock};
  config.persistence.initialize = false;
  EXPECT_FALSE(second.Init(config).ok());
  EXPECT_NE(server.port(), 0U);
  Connect(11);
  EXPECT_NE(peer.generation, 0U);
}

TEST_F(TradingdPersistenceTest, MissingStateRequiresExplicitInitialization) {
  config.persistence.initialize = false;
  EXPECT_FALSE(server.Init(config).ok());
  config.persistence.initialize = true;
  EXPECT_TRUE(server.Init(config).ok()) << server.error_detail();
}

TEST_F(TradingdPersistenceTest, InitializationRefusesExistingState) {
  Start();
  server.Close();
  EXPECT_FALSE(server.Init(config).ok());
  config.persistence.initialize = false;
  EXPECT_TRUE(server.Init(config).ok()) << server.error_detail();
}

TEST_F(TradingdPersistenceTest, AccountIdentityMismatchRefusesStartup) {
  Start();
  server.Close();
  config.persistence.initialize = false;
  ::yquant::base::FormatTo(config.persistence.account_key,
                           sizeof(config.persistence.account_key),
                           "simulation/account-B/CNY");
  EXPECT_FALSE(server.Init(config).ok());
}

TEST_F(TradingdPersistenceTest, ChangedPriceTickRefusesRecovery) {
  Start();
  Connect(11);
  ASSERT_NE(peer.generation, 0U);
  server.Close();
  config.persistence.initialize = false;
  auto& instrument = config.market_data.instruments[0];
  instrument.instrument.price_tick = 2;
  // Keep the mock source valid so the frozen identity check rejects recovery.

  EXPECT_EQ(server.Init(config).code, base::ErrorCode::kCorrupt);
  EXPECT_NE(std::strstr(server.error_detail(), "profile mismatch"), nullptr);
}

TEST_F(TradingdPersistenceTest, ChangedQuantityStepRefusesRecovery) {
  Start();
  server.Close();
  config.persistence.initialize = false;
  config.market_data.instruments[0].instrument.quantity_step = 2;
  EXPECT_EQ(server.Init(config).code, base::ErrorCode::kCorrupt);
  EXPECT_NE(std::strstr(server.error_detail(), "profile mismatch"), nullptr);
}

TEST_F(TradingdPersistenceTest, SymbolPaddingDoesNotChangeIdentity) {
  Start();
  server.Close();
  config.persistence.initialize = false;
  auto& definition = config.market_data.instruments[0].instrument;
  const auto terminator = definition.symbol_view().size();
  ASSERT_LT(terminator + 1, sizeof(definition.symbol));
  definition.symbol[terminator + 1] = 'x';
  EXPECT_TRUE(server.Init(config).ok()) << server.error_detail();
}

TEST_F(TradingdPersistenceTest, ChangedInstrumentTypeRefusesRecovery) {
  Start();
  server.Close();
  config.persistence.initialize = false;
  config.market_data.instruments[0].instrument.type =
      model::InstrumentType::kFutures;
  // An equity identity cannot be reinterpreted as a futures contract. The
  // validated catalog rejects this before the persisted profile is installed.
  EXPECT_EQ(server.Init(config).code, base::ErrorCode::kInvalidArgument);
  EXPECT_NE(std::strstr(server.error_detail(), "instrument_type"), nullptr);
}

TEST_F(TradingdPersistenceTest, CorruptCurrentRefusesStartup) {
  Start();
  server.Close();
  auto* file = std::fopen(StateFile("CURRENT").c_str(), "wb");
  ASSERT_NE(file, nullptr);
  ASSERT_EQ(std::fwrite("corrupt", 1, 7, file), 7U);
  ASSERT_EQ(std::fclose(file), 0);
  config.persistence.initialize = false;
  EXPECT_FALSE(server.Init(config).ok());
}

TEST_F(TradingdPersistenceTest, CorruptSnapshotRefusesStartup) {
  Start();
  server.Close();
  bool changed = false;
  for (const auto& entry : ::yquant::testing::files::Entries(
           ::yquant::testing::files::FilePath(config.persistence.directory))) {
    if (!entry.path().Filename().string().starts_with("snapshot-")) {
      continue;
    }
    auto* file = std::fopen(entry.path().c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite("corrupt", 1, 7, file), 7U);
    ASSERT_EQ(std::fclose(file), 0);
    changed = true;
  }
  ASSERT_TRUE(changed);
  config.persistence.initialize = false;
  EXPECT_FALSE(server.Init(config).ok());
}

TEST_F(TradingdPersistenceTest, MissingIdentityRefusesStartup) {
  Start();
  server.Close();
  ASSERT_TRUE(::yquant::testing::files::RemoveFile(
      ::yquant::testing::files::FilePath(StateFile("IDENTITY"))));
  config.persistence.initialize = false;
  EXPECT_FALSE(server.Init(config).ok());
}
TEST_F(TradingdPersistenceTest,
       FinishRequiresDrainAndPublishesFinalCheckpoint) {
  Start();
  Connect(11);
  Order();
  Advance();
  EXPECT_EQ(server.Finish().code, base::ErrorCode::kInvalidState);
  server.BeginStopping(clock.now);
  Pump();
  for (int attempt = 0; attempt < 8 && !server.is_drained(); ++attempt) {
    Advance();
  }
  ASSERT_TRUE(server.is_drained());
  ASSERT_TRUE(server.Finish().ok()) << server.error_detail();
  EXPECT_EQ(server.account().cash.balance, 1000000000);
  EXPECT_EQ(server.account().cash.reserved, 0);
  Restart();
  EXPECT_EQ(server.account().cash.balance, 1000000000);
  EXPECT_EQ(server.account().cash.reserved, 0);
}

TEST_F(TradingdPersistenceTest,
       PeriodicCheckpointReplaysOnlyFollowingWalSuffix) {
  config.trading.mode = trading::MockTradingMode::kFill;
  config.persistence.snapshot_interval_ms = 1000;
  Start();
  Connect(11);
  const auto checkpoint = [&](size_t expected) {
    clock.now += 1000000000;
    Pump();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool published = false;
    while (std::chrono::steady_clock::now() < deadline && !published) {
      Pump();
      size_t count = 0;
      for (const auto& entry :
           ::yquant::testing::files::Entries(::yquant::testing::files::FilePath(
               config.persistence.directory))) {
        count += entry.path().Filename().string().starts_with("snapshot-") &&
                 entry.path().Extension() == ".bin";
      }
      published = count >= expected;
      if (!published) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    EXPECT_TRUE(published);
  };
  Order();
  Advance();
  Advance();
  Advance();
  ASSERT_EQ(server.account().cash.balance, 900000000);
  checkpoint(2);
  Order(2, 5);
  Advance();
  Advance();
  Advance();
  ASSERT_EQ(server.account().cash.balance, 850000000);
  checkpoint(3);
  Order(3, 5);
  Advance();
  Advance();
  Advance();
  ASSERT_EQ(server.account().cash.balance, 800000000);
  Restart();
  EXPECT_EQ(server.account().cash.balance, 800000000);
  EXPECT_EQ(server.account().cash.reserved, 0);
  ASSERT_EQ(server.account().position_count, 1U);
  EXPECT_EQ(server.account().positions[0].total_qty, 20);
}

// Simulates loss of complete trailing WAL records after process shutdown while
// retaining immutable segment headers and file size. Each test uses a fresh
// stream small enough to fit its records in the first segment.
size_t KeepWalRecords(const char* directory, size_t keep) {
  constexpr size_t kTestSegmentHeaderBytes = 64;
  size_t records = 0;
  for (const auto& entry : ::yquant::testing::files::RecursiveEntries(
           ::yquant::testing::files::FilePath(directory))) {
    if (entry.path().Extension() != ".wal") {
      continue;
    }
    auto* file = std::fopen(entry.path().c_str(), "r+b");
    if (file == nullptr) {
      return SIZE_MAX;
    }
    const auto size = ::yquant::testing::files::FileSize(
        ::yquant::testing::files::FilePath(entry.path()));
    std::vector<unsigned char> bytes(size);
    if (std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
      std::fclose(file);
      return SIZE_MAX;
    }
    size_t offset = kTestSegmentHeaderBytes;
    size_t cutoff = kTestSegmentHeaderBytes;
    while (offset + 4 <= bytes.size()) {
      const auto length = static_cast<uint32_t>(bytes[offset]) |
                          (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                          (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                          (static_cast<uint32_t>(bytes[offset + 3]) << 24);
      if (length == 0) {
        break;
      }
      if (length > bytes.size() - offset || length < 48) {
        std::fclose(file);
        return SIZE_MAX;
      }
      ++records;
      offset += length;
      if (records <= keep) {
        cutoff = offset;
      }
    }
    std::fill(bytes.begin() + cutoff, bytes.end(), 0);
    const bool written =
        std::fseek(file, 0, SEEK_SET) == 0 &&
        std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (std::fclose(file) != 0 || !written) {
      return SIZE_MAX;
    }
  }
  return records;
}

TEST_F(TradingdPersistenceTest,
       EveryWorkingOrderWalPrefixRecoversWithoutResend) {
  size_t record_count = 64;
  for (size_t keep = 0; keep <= record_count; ++keep) {
    SCOPED_TRACE(keep);
    server.Close();
    peer.connection.Close();
    peer.messages.clear();
    peer.generation = 0;
    ::yquant::base::FormatTo(config.persistence.directory,
                             sizeof(config.persistence.directory), "{}/cut-{}",
                             directory.path(), keep);
    config.persistence.initialize = true;
    Start();
    Connect(11);
    Order();
    Advance();
    ASSERT_NE(Update(model::OrderUpdateKind::kAccepted), nullptr);
    trading::Message cancel;
    cancel.type = trading::MessageType::kCancelOrder;
    cancel.generation = peer.generation;
    cancel.strategy_order_id = {1};
    cancel.cancel_id = {1};
    ASSERT_TRUE(peer.connection.Queue(cancel).ok());
    Pump();
    ASSERT_EQ(server.account().cash.reserved, 100000000);
    server.Close();
    peer.connection.Close();
    const auto count = KeepWalRecords(config.persistence.directory, keep);
    ASSERT_NE(count, SIZE_MAX);
    ASSERT_GE(count, 7U);
    ASSERT_LE(count, 64U);
    if (keep == 0) {
      record_count = count;
    } else {
      ASSERT_EQ(count, record_count);
    }
    config.persistence.initialize = false;
    ASSERT_TRUE(server.Init(config).ok()) << server.error_detail();
    EXPECT_EQ(server.account().cash.balance, 1000000000);
    EXPECT_EQ(server.account().cash.reserved, 0);
    Connect(22);
    ASSERT_NE(peer.generation, 0U);
    Order();
    Advance();
    EXPECT_NE(Update(model::OrderUpdateKind::kAccepted), nullptr);
    EXPECT_EQ(server.account().cash.reserved, 100000000);
    EXPECT_EQ(server.account().cash.balance, 1000000000);
  }
}

class PersistenceConfigTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory.valid());
    ASSERT_TRUE(directory.Write(
        "market.json",
        R"({"adapter":"shm","source_id":1,"runtime_directory":"/tmp/yquant","stream":"test",)"
        R"("instrument_catalog":"instruments.json","instruments":[)"
        R"({"venue":"SSE","symbol":"600000"}]})"));
    ASSERT_TRUE(directory.Write(
        "broker.json",
        R"({"adapter":"mock","mode":"working","report_interval_ms":1,)"
        R"("shutdown_timeout_ms":2000,"initial_cash":1000000000,"initial_positions":[]})"));
  }
  base::Status Load(std::string_view persistence) {
    if (!directory.Write(
            "server.json",
            R"({"listen":{"address":"127.0.0.1","port":12345},)"
            R"("market_data_config":"market.json","trading_config":"broker.json",)"
            R"("token":"test-token","persistence":)" +
                std::string(persistence) + "}")) {
      return {base::ErrorCode::kIoError, {}};
    }
    ::yquant::testing::ScopedWorkingDirectory cwd(directory.path());
    if (!cwd.valid()) {
      return {base::ErrorCode::kIoError, {}};
    }
    return loader.Load(directory.File("server.json").c_str());
  }
  RuntimeDirectory directory;
  ConfigLoader loader;
};

TEST_F(PersistenceConfigTest, ResolvesRelativeDirectoryWithoutCreatingState) {
  ASSERT_TRUE(
      Load(R"({"directory":"state","account_key":"simulation/account-A/CNY"})")
          .ok())
      << loader.error().message;
  EXPECT_EQ(std::string(loader.config().persistence.directory),
            ::yquant::testing::files::CanonicalPath(
                ::yquant::testing::files::FilePath(directory.File("state")))
                .string());
  EXPECT_FALSE(loader.config().persistence.initialize);
  EXPECT_FALSE(::yquant::testing::files::Exists(
      ::yquant::testing::files::FilePath(directory.File("state"))));
}

TEST_F(PersistenceConfigTest, RejectsUnknownAndMissingFields) {
  for (const auto* block :
       {R"({"directory":"state","account_key":"A","unknown":1})",
        R"({"directory":"state","account_key":"A","initialize":1})",
        R"({"directory":"state"})", R"({"account_key":"A"})"}) {
    SCOPED_TRACE(block);
    EXPECT_EQ(Load(block).code, base::ErrorCode::kInvalidArgument);
  }
}

TEST_F(PersistenceConfigTest, RejectsOutOfRangeStorageOptions) {
  for (const auto* option :
       {R"("wal_segment_bytes":2097152)", R"("wal_segment_bytes":6291456)",
        R"("reserve_segments":1)", R"("reserve_segments":17)",
        R"("sync_interval_ms":0)", R"("sync_interval_ms":1001)",
        R"("sync_after_bytes":4095)", R"("sync_after_bytes":67108865)",
        R"("snapshot_interval_ms":999)", R"("snapshot_interval_ms":3600001)"}) {
    SCOPED_TRACE(option);
    EXPECT_EQ(Load(std::string(R"({"directory":"state","account_key":"A",)") +
                   option + "}")
                  .code,
              base::ErrorCode::kInvalidArgument);
  }
}

// Waits on the live wire; this helper neither advances a simulated clock nor
// makes direct calls into the child daemon's ledger.
bool ReadProcessMessage(trading::Connection& connection,
                        trading::Message& message) {
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

TEST(TradingdPersistenceProcessTest, SigkillRecoversFileBackedPageCache) {
  RuntimeDirectory directory;
  ASSERT_TRUE(directory.valid());
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(socket, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof(address);
  ASSERT_EQ(::bind(socket, reinterpret_cast<sockaddr*>(&address), size), 0);
  ASSERT_EQ(::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size),
            0);
  const auto port = ntohs(address.sin_port);
  ::close(socket);
  ASSERT_TRUE(directory.Write(
      "market.json",
      R"({"adapter":"shm","source_id":1,"runtime_directory":"/tmp/yquant","stream":"test",)"
      R"("instrument_catalog":"instruments.json","instruments":[)"
      R"({"venue":"SSE","symbol":"600000"}]})"));
  ASSERT_TRUE(directory.Write(
      "broker.json",
      R"({"adapter":"mock","mode":"fill","report_interval_ms":1,)"
      R"("shutdown_timeout_ms":2000,"initial_cash":1000000000,"initial_positions":[]})"));
  ASSERT_TRUE(directory.Write(
      "server.json",
      R"({"listen":{"address":"127.0.0.1","port":)" + std::to_string(port) +
          R"(},"market_data_config":"market.json","trading_config":"broker.json",)"
          R"("token":"persistence-token","heartbeat_timeout_ms":3000,)"
          R"("shutdown_timeout_ms":2000,"persistence":{"directory":"state",)"
          R"("account_key":"simulation/account-A/CNY","wal_segment_bytes":4194304,)"
          R"("sync_interval_ms":1000,"sync_after_bytes":4194304}})"));
  ::yquant::testing::ScopedWorkingDirectory cwd(directory.path());
  ASSERT_TRUE(cwd.valid());
  testing::ChildProcess original;
  ASSERT_EQ(original.Start(YQUANT_TEST_TRADINGD_PATH,
                           {"--config", directory.File("server.json"),
                            "--initialize-state"}),
            0);
  ASSERT_TRUE(original.WaitForOutput("Running")) << original.output();
  trading::Endpoint endpoint;
  endpoint.port = port;
  trading::Connection peer;
  ASSERT_TRUE(peer.Connect(endpoint, 1000000000).ok());
  trading::Message hello;
  hello.type = trading::MessageType::kHello;
  hello.client_id = 11;
  ::yquant::base::FormatTo(hello.token, sizeof(hello.token),
                           "persistence-token");
  hello.instrument_count = 1;
  ASSERT_EQ(model::MakeInstrumentId("SSE", "600000", hello.instruments[0].id),
            base::ErrorCode::kOk);
  hello.instruments[0].price_decimals = 2;
  ASSERT_TRUE(peer.Queue(hello).ok());
  trading::Message message;
  ASSERT_TRUE(ReadProcessMessage(peer, message));
  ASSERT_EQ(message.type, trading::MessageType::kWelcome);
  const auto generation = message.generation;
  trading::Message order;
  order.type = trading::MessageType::kNewOrder;
  order.generation = generation;
  order.strategy_order_id = {1};
  order.request = {hello.instruments[0].id, model::OrderSide::kBuy, 1000, 10};
  ASSERT_TRUE(peer.Queue(order).ok());
  bool filled = false;
  for (int i = 0; i < 16 && !filled; ++i) {
    ASSERT_TRUE(ReadProcessMessage(peer, message));
    filled = message.type == trading::MessageType::kOrderUpdate &&
             message.update.order.state == model::OrderState::kFilled;
  }
  ASSERT_TRUE(filled);
  ASSERT_EQ(message.account.cash.balance, 900000000);
  ASSERT_TRUE(original.Signal(SIGKILL));
  ASSERT_TRUE(original.Wait());
  peer.Close();
  testing::ChildProcess recovered;
  ASSERT_EQ(recovered.Start(YQUANT_TEST_TRADINGD_PATH,
                            {"--config", directory.File("server.json")}),
            0);
  ASSERT_TRUE(recovered.WaitForOutput("Running")) << recovered.output();
  ASSERT_TRUE(peer.Connect(endpoint, 1000000000).ok());
  hello.client_id = 22;
  ASSERT_TRUE(peer.Queue(hello).ok());
  ASSERT_TRUE(ReadProcessMessage(peer, message));
  ASSERT_EQ(message.type, trading::MessageType::kWelcome);
  EXPECT_NE(message.generation, generation);
  EXPECT_EQ(message.account.cash.balance, 900000000);
  EXPECT_EQ(message.account.cash.reserved, 0);
  ASSERT_EQ(message.account.position_count, 1U);
  EXPECT_EQ(message.account.positions[0].total_qty, 10);
  peer.Close();
  ASSERT_TRUE(recovered.Signal(SIGTERM));
  ASSERT_TRUE(recovered.Wait()) << recovered.output();
  EXPECT_EQ(recovered.exit_code(), 0) << recovered.output();
}

}  // namespace
}  // namespace yquant::tradingd
