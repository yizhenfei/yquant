#include <stdint.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "marketd/test_support.h"
#include "tradingd/server.h"
#include "yquant/base/format.h"
#include "yquant/trading/connection.h"

#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
#include <mysql.h>
#endif

namespace yquant::tradingd {
namespace {

#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
class MysqlServerClock final : public base::MonotonicClock {
 public:
  uint64_t Now() const noexcept override { return now; }
  uint64_t now = 1000000;
};

// Exercises the real socket owner and background database worker. SQL reads
// select this test's fresh ledger; cleanup never touches another test's rows.
class MysqlServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* database = std::getenv("YQUANT_MYSQL_TEST_DATABASE");
    if (database == nullptr || database[0] == '\0') {
      GTEST_SKIP() << "YQUANT_MYSQL_TEST_DATABASE is not configured";
    }
    ASSERT_TRUE(directory.valid());
    const char* host = std::getenv("YQUANT_MYSQL_TEST_HOST");
    if (host == nullptr || host[0] == '\0') {
      host = "127.0.0.1";
    }
    const char* user = std::getenv("YQUANT_MYSQL_TEST_USER");
    if (user == nullptr || user[0] == '\0') {
      user = "root";
    }
    const char* password = std::getenv("YQUANT_MYSQL_TEST_PASSWORD");
    const char* port = std::getenv("YQUANT_MYSQL_TEST_PORT");
    config.listen.port = 0;
    ::yquant::base::FormatTo(config.token, sizeof(config.token),
                             "mysql-test-token");
    config.market_data = marketd::testing::ScopeConfig();
    config.trading.mode = trading::MockTradingMode::kFill;
    config.trading.initial_account.cash_balance = 1000000000;
    config.heartbeat_timeout_ns = kMaximumTimeoutNs;
    ::yquant::base::FormatTo(config.persistence.directory,
                             sizeof(config.persistence.directory), "{}/state",
                             directory.path());
    ::yquant::base::FormatTo(config.persistence.account_key,
                             sizeof(config.persistence.account_key),
                             "mysql-server:{}", directory.path());
    config.persistence.wal_segment_bytes = 4 * 1024 * 1024;
    config.persistence.sync_interval_ms = 1;
    config.persistence.initialize = true;
    auto& archive = config.mysql_archive;
    archive.enabled = true;
    ::yquant::base::FormatTo(archive.host, sizeof(archive.host), "{}", host);
    archive.port = static_cast<uint16_t>(
        port != nullptr ? std::strtoul(port, nullptr, 10) : 3306);
    ::yquant::base::FormatTo(archive.database, sizeof(archive.database), "{}",
                             database);
    ::yquant::base::FormatTo(archive.user, sizeof(archive.user), "{}", user);
    ::yquant::base::FormatTo(archive.password_env, sizeof(archive.password_env),
                             "YQUANT_MYSQL_TEST_PASSWORD");
    archive.timeout_seconds = 1;
    archive.poll_ms = 1;
    archive.retry_ms = 10;
    archive.shutdown_ms = 1000;
    archive.min_free_bytes = 0;

    connection = mysql_init(nullptr);
    ASSERT_NE(connection, nullptr);
    unsigned timeout = 1;
    ASSERT_EQ(mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout),
              0);
    ASSERT_NE(mysql_real_connect(connection, host, user,
                                 password != nullptr ? password : "", database,
                                 archive.port, nullptr, 0),
              nullptr)
        << mysql_error(connection);
  }

  void TearDown() override {
    peer.Close();
    server.Close();
    if (connection == nullptr) {
      return;
    }
    const auto rows =
        Rows("SELECT HEX(ledger_id) FROM ledgers WHERE account_key=" +
             Quote(config.persistence.account_key));
    for (const auto& row : rows) {
      if (row.empty()) {
        continue;
      }
      const auto where = " WHERE ledger_id=UNHEX('" + row[0] + "')";
      for (const char* table :
           {"account_positions", "account_cash", "order_updates", "fills",
            "orders", "archive_progress", "archive_gaps", "trade_events",
            "strategy_sessions", "archive_runs", "ledgers"}) {
        (void)mysql_query(
            connection, (std::string("DELETE FROM ") + table + where).c_str());
      }
    }
    mysql_close(connection);
    connection = nullptr;
  }

  std::string Quote(const char* text) {
    std::string escaped(2 * std::strlen(text) + 1, '\0');
    const auto size = mysql_real_escape_string(connection, escaped.data(), text,
                                               std::strlen(text));
    escaped.resize(size);
    return "'" + escaped + "'";
  }

  std::vector<std::vector<std::string>> Rows(const std::string& sql) {
    std::vector<std::vector<std::string>> rows;
    if (mysql_query(connection, sql.c_str()) != 0) {
      ADD_FAILURE() << mysql_error(connection) << "; SQL: " << sql;
      return rows;
    }
    MYSQL_RES* result = mysql_store_result(connection);
    if (result == nullptr) {
      ADD_FAILURE() << mysql_error(connection);
      return rows;
    }
    const auto columns = mysql_num_fields(result);
    while (MYSQL_ROW row = mysql_fetch_row(result)) {
      const auto* sizes = mysql_fetch_lengths(result);
      std::vector<std::string> fields;
      for (unsigned i = 0; i < columns; ++i) {
        fields.emplace_back(row[i] != nullptr ? row[i] : "",
                            row[i] != nullptr ? sizes[i] : 0);
      }
      rows.push_back(std::move(fields));
    }
    mysql_free_result(result);
    return rows;
  }

  int64_t Number(const std::string& sql) {
    const auto rows = Rows(sql);
    if (rows.empty() || rows[0].empty() || rows[0][0].empty()) {
      return -1;
    }
    return std::strtoll(rows[0][0].c_str(), nullptr, 10);
  }

  std::string LedgerWhere() {
    return "ledger_id IN (SELECT ledger_id FROM ledgers WHERE account_key=" +
           Quote(config.persistence.account_key) + ")";
  }

  void Start() {
    ASSERT_TRUE(server.Init(config).ok()) << server.error_detail();
  }

  void Connect(uint64_t client_id = 41) {
    peer.Close();
    updates.clear();
    generation = 0;
    trading::Endpoint endpoint;
    endpoint.port = server.port();
    ASSERT_TRUE(peer.Connect(endpoint, 1000000000).ok());
    trading::Message hello;
    hello.type = trading::MessageType::kHello;
    hello.client_id = client_id;
    ::yquant::base::FormatTo(hello.token, sizeof(hello.token),
                             "mysql-test-token");
    hello.instrument_count = 1;
    hello.instruments[0] = {config.market_data.instruments[0].instrument.id, 2};
    ASSERT_TRUE(peer.Queue(hello).ok());
    for (int i = 0; i < 20 && generation == 0; ++i) {
      Pump();
    }
    ASSERT_NE(generation, 0);
  }

  void Pump() {
    clock.now += 1000000;
    for (int i = 0; i < 20; ++i) {
      if (peer.open()) {
        (void)peer.Service();
      }
      const auto status = server.Service(clock.Now());
      ASSERT_TRUE(status.ok()) << server.error_detail();
      while (peer.open()) {
        trading::Message message;
        const auto result = peer.Poll(message);
        if (result.state != trading::ReceiveState::kMessage) {
          break;
        }
        if (message.type == trading::MessageType::kWelcome) {
          generation = message.generation;
        }
        if (message.type == trading::MessageType::kOrderUpdate) {
          updates.push_back(message);
        }
      }
      std::this_thread::yield();
    }
  }

  void Order(uint64_t local_id = 1, int64_t quantity = 10) {
    trading::Message message;
    message.type = trading::MessageType::kNewOrder;
    message.generation = generation;
    message.strategy_order_id = {local_id};
    message.request = {config.market_data.instruments[0].instrument.id,
                       model::OrderSide::kBuy, 1000, quantity};
    ASSERT_TRUE(peer.Queue(message).ok());
    for (int i = 0; i < 10; ++i) {
      Pump();
    }
  }

  template <typename Predicate>
  bool Wait(Predicate predicate) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      Pump();
      if (HasFatalFailure()) {
        return false;
      }
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
  }

  bool HasUpdate(uint64_t order, model::OrderState state) const {
    for (const auto& message : updates) {
      if (message.update.order.strategy_order_id.value == order &&
          message.update.order.state == state) {
        return true;
      }
    }
    return false;
  }

  marketd::testing::RuntimeDirectory directory;
  MysqlServerClock clock;
  ServerConfig config;
  Server server{&clock};
  trading::Connection peer;
  uint64_t generation = 0;
  std::vector<trading::Message> updates;
  MYSQL* connection = nullptr;
};

TEST_F(MysqlServerTest, ArchivesSocketFillsAndLocalRejectionsWithAccount) {
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  Order(2, 1000);
  ASSERT_TRUE(HasUpdate(2, model::OrderState::kRejected));
  const auto where = LedgerWhere();
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM orders WHERE " + where) == 2 &&
           Number("SELECT COUNT(*) FROM fills WHERE " + where) == 2 &&
           Number("SELECT balance FROM account_cash WHERE " + where) ==
               server.account().cash.balance;
  }));
  EXPECT_EQ(server.account().cash.balance, 900000000);
  EXPECT_EQ(Number("SELECT SUM(quantity) FROM fills WHERE " + where), 10);
  EXPECT_EQ(Number("SELECT available FROM account_cash WHERE " + where),
            server.account().cash.available);
  EXPECT_EQ(Number("SELECT reserved FROM account_cash WHERE " + where), 0);
  EXPECT_EQ(Number("SELECT total_qty FROM account_positions WHERE " + where),
            10);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM orders WHERE " + where +
                   " AND core_order_id IS NULL AND status=4"),
            1);
  EXPECT_GT(Number("SELECT COUNT(*) FROM trade_events WHERE " + where +
                   " AND record_type=2"),
            0);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM order_updates WHERE " + where +
                   " AND update_kind=1"),
            1);
}

TEST_F(MysqlServerTest, RestartInstallsRecoveredBaselineAfterUnfinishedClose) {
  config.trading.mode = trading::MockTradingMode::kWorking;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kWorking));
  const auto where = LedgerWhere();
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM orders WHERE " + where +
                  " AND status=1") == 1;
  }));
  EXPECT_EQ(server.account().cash.reserved, 100000000);
  // Close retains an unfinished working order and omits the orderly end file.
  peer.Close();
  server.Close();
  config.persistence.initialize = false;
  clock.now += 1000000;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(server.account().cash.reserved, 0);
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM archive_runs WHERE " + where) == 2 &&
           Number("SELECT COUNT(*) FROM orders WHERE " + where +
                  " AND status=3 AND source_lsn=0") == 1;
  }));
  EXPECT_EQ(Number("SELECT reserved FROM account_cash WHERE " + where), 0);
  const auto identity =
      Rows("SELECT HEX(ledger_id) FROM ledgers WHERE account_key=" +
           Quote(config.persistence.account_key));
  ASSERT_EQ(identity.size(), 1);
  const auto ledger = "UNHEX('" + identity[0][0] + "')";
  EXPECT_EQ(Number("SELECT COUNT(*) FROM archive_runs newer JOIN archive_runs "
                   "older ON newer.ledger_id=older.ledger_id AND "
                   "newer.previous_run_id=older.run_id WHERE newer.ledger_id=" +
                   ledger + " AND newer.previous_end_lsn=older.final_lsn"),
            1);
  Connect(42);
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kWorking));
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM orders WHERE " + where) == 2 &&
           Number("SELECT COUNT(*) FROM strategy_sessions WHERE " + where) == 2;
  }));
}

TEST_F(MysqlServerTest, EnablingArchiveOnExistingStateRecordsHistoricalGap) {
  config.mysql_archive.enabled = false;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  peer.Close();
  server.Close();
  config.persistence.initialize = false;
  config.mysql_archive.enabled = true;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  const auto where = LedgerWhere();
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM archive_runs WHERE " + where) == 1 &&
           Number("SELECT COUNT(*) FROM archive_gaps WHERE " + where) == 1 &&
           Number("SELECT balance FROM account_cash WHERE " + where) ==
               900000000;
  }));
  EXPECT_EQ(Number("SELECT COUNT(*) FROM orders WHERE " + where +
                   " AND source_lsn=0 AND status=2"),
            1);
  EXPECT_EQ(Number("SELECT SUM(quantity) FROM fills WHERE " + where), 10);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM fills WHERE " + where +
                   " AND baseline_discovered=1"),
            2);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM trade_events WHERE " + where), 0);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM archive_runs WHERE " + where +
                   " AND history_gap=1 AND previous_run_id=0"),
            1);
}

TEST_F(MysqlServerTest, ReenablingArchiveSkipsDisabledRunAndPreservesHistory) {
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  const auto where = LedgerWhere();
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM fills WHERE " + where) == 2 &&
           Number("SELECT balance FROM account_cash WHERE " + where) ==
               900000000;
  }));
  peer.Close();
  server.Close();

  config.persistence.initialize = false;
  config.mysql_archive.enabled = false;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect(42);
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  EXPECT_EQ(server.account().cash.balance, 800000000);
  peer.Close();
  server.Close();

  config.mysql_archive.enabled = true;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM archive_runs WHERE " + where) == 2 &&
           Number("SELECT COUNT(*) FROM archive_gaps WHERE " + where) == 1 &&
           Number("SELECT COUNT(*) FROM fills WHERE " + where) == 4 &&
           Number("SELECT balance FROM account_cash WHERE " + where) ==
               800000000;
  }));
  EXPECT_EQ(Number("SELECT COUNT(*) FROM orders WHERE " + where), 2);
  EXPECT_EQ(Number("SELECT SUM(quantity) FROM fills WHERE " + where), 20);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM fills WHERE " + where +
                   " AND baseline_discovered=1"),
            2);
  EXPECT_EQ(
      Number("SELECT COUNT(DISTINCT run_id) FROM trade_events WHERE " + where),
      1);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM strategy_sessions WHERE " + where), 2);
  EXPECT_EQ(Number("SELECT COUNT(*) FROM archive_runs WHERE " + where +
                   " AND final_lsn IS NOT NULL"),
            1);
}

TEST_F(MysqlServerTest, FinishSealsRunAndRestartContinuesArchiving) {
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  server.BeginStopping(clock.Now());
  ASSERT_TRUE(Wait([&] { return server.is_drained(); }));
  ASSERT_TRUE(server.Finish().ok()) << server.error_detail();
  const auto where = LedgerWhere();
  EXPECT_EQ(Number("SELECT COUNT(*) FROM archive_runs WHERE " + where +
                   " AND final_lsn IS NOT NULL AND final_lsn>0"),
            1);
  const auto identity =
      Rows("SELECT HEX(ledger_id) FROM ledgers WHERE account_key=" +
           Quote(config.persistence.account_key));
  ASSERT_EQ(identity.size(), 1);
  const auto ledger = "UNHEX('" + identity[0][0] + "')";
  EXPECT_EQ(Number("SELECT COUNT(*) FROM archive_runs r JOIN archive_progress "
                   "p ON r.ledger_id=p.ledger_id AND r.run_id=p.run_id "
                   "WHERE r.ledger_id=" +
                   ledger + " AND r.final_lsn=p.lsn"),
            1);
  peer.Close();
  server.Close();
  config.persistence.initialize = false;
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect(42);
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  ASSERT_TRUE(Wait([&] {
    return Number("SELECT COUNT(*) FROM archive_runs WHERE " + where) == 2 &&
           Number("SELECT SUM(quantity) FROM fills WHERE " + where) == 20 &&
           Number("SELECT balance FROM account_cash WHERE " + where) ==
               800000000;
  }));
}

TEST_F(MysqlServerTest, UnreachableDatabaseDoesNotBlockSocketTrading) {
  config.mysql_archive.port = 1;
  const auto before = std::chrono::steady_clock::now();
  Start();
  ASSERT_FALSE(HasFatalFailure());
  Connect();
  ASSERT_FALSE(HasFatalFailure());
  Order();
  ASSERT_TRUE(HasUpdate(1, model::OrderState::kFilled));
  EXPECT_EQ(server.account().cash.balance, 900000000);
  EXPECT_LT(std::chrono::steady_clock::now() - before, std::chrono::seconds(2));
}
#else
TEST(MysqlServerTest, RequiresMysqlBuild) {
  GTEST_SKIP() << "MySQL client support is disabled";
}
#endif

}  // namespace
}  // namespace yquant::tradingd
