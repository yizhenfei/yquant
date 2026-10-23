#include "tradingd/mysql_archive_store.h"

#include "trading_engine_harness.h"
#include "tradingd/archive_projection.h"

#if defined(YQUANT_WITH_MYSQL) && YQUANT_WITH_MYSQL
#include <arpa/inet.h>
#include <mysql.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>
#include <vector>

#include "../marketd/test_support.h"
#include "gtest/gtest.h"
#include "tradingd/state_codec.h"
#include "yquant/trading/mock_trading_adapter.h"

namespace yquant::tradingd {
namespace {
using base::ErrorCode;

class ArchiveClock final : public base::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return 1000; }
};

// Captures the same owned facts that gateway WAL carries.
class FactSink final : public trading::TradingMutationSink {
 public:
  base::Status BeforeCommit(
      const trading::TradingMutation& mutation) noexcept override {
    facts.push_back(mutation);
    return {};
  }
  std::vector<trading::TradingMutation> facts;
};

// A real TCP relay strips the optional SSL capability from the greeting so
// the test can identify COMMIT. It forwards the transaction to MySQL, receives
// its actual OK packet, then disconnects the client without forwarding that OK.
// This exercises a successful server commit with an unknown client outcome.
class CommitReplyProxy {
 public:
  explicit CommitReplyProxy(std::uint16_t upstream_port) {
    listener_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_ < 0) return;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener_, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0 ||
        listen(listener_, 1) != 0)
      return;
    socklen_t length = sizeof(address);
    if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address),
                    &length) != 0)
      return;
    port_ = ntohs(address.sin_port);
    worker_ = std::thread([this, upstream_port] { Run(upstream_port); });
  }
  ~CommitReplyProxy() {
    if (listener_ >= 0) shutdown(listener_, SHUT_RDWR);
    const auto client = client_.load();
    const auto server = server_.load();
    if (client >= 0) shutdown(client, SHUT_RDWR);
    if (server >= 0) shutdown(server, SHUT_RDWR);
    if (worker_.joinable()) worker_.join();
    if (listener_ >= 0) close(listener_);
  }
  std::uint16_t port() const { return port_; }
  bool dropped_commit_ok() const { return dropped_.load(); }

 private:
  static bool Read(int fd, void* storage, std::size_t length) {
    auto* bytes = static_cast<char*>(storage);
    for (std::size_t offset = 0; offset < length;) {
      const auto count = recv(fd, bytes + offset, length - offset, 0);
      if (count <= 0) return false;
      offset += count;
    }
    return true;
  }
  static bool Packet(int fd, std::vector<unsigned char>& packet) {
    std::array<unsigned char, 4> header;
    if (!Read(fd, header.data(), header.size())) return false;
    const std::size_t length = header[0] | (std::size_t(header[1]) << 8) |
                               (std::size_t(header[2]) << 16);
    packet.resize(4 + length);
    std::memcpy(packet.data(), header.data(), 4);
    return Read(fd, packet.data() + 4, length);
  }
  static bool Send(int fd, const std::vector<unsigned char>& packet) {
    for (std::size_t offset = 0; offset < packet.size();) {
#ifdef MSG_NOSIGNAL
      constexpr int kSendFlags = MSG_NOSIGNAL;
#else
      constexpr int kSendFlags = 0;
#endif
      const auto count =
          send(fd, packet.data() + offset, packet.size() - offset, kSendFlags);
      if (count <= 0) return false;
      offset += count;
    }
    return true;
  }
  void Run(std::uint16_t upstream_port) {
    const int client = accept(listener_, nullptr, nullptr);
    client_.store(client);
    if (client < 0) return;
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    server_.store(server);
    timeval timeout{5, 0};
    for (int fd : {client, server}) {
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
      const int one = 1;
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    sockaddr_in upstream{};
    upstream.sin_family = AF_INET;
    upstream.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    upstream.sin_port = htons(upstream_port);
    if (server >= 0 && connect(server, reinterpret_cast<sockaddr*>(&upstream),
                               sizeof(upstream)) == 0) {
      Relay(client, server);
    }
    client_.store(-1);
    server_.store(-1);
    close(client);
    if (server >= 0) close(server);
  }
  void Relay(int client, int server) {
    std::vector<unsigned char> packet;
    if (!Packet(server, packet)) return;
    std::size_t version_end = 5;
    while (version_end < packet.size() && packet[version_end] != 0)
      ++version_end;
    const auto capabilities = version_end + 14;
    if (capabilities + 1 >= packet.size()) return;
    packet[capabilities + 1] &= static_cast<unsigned char>(~0x08);
    if (!Send(client, packet)) return;
    bool committing = false;
    for (;;) {
      fd_set ready;
      FD_ZERO(&ready);
      FD_SET(client, &ready);
      FD_SET(server, &ready);
      timeval timeout{5, 0};
      if (select(std::max(client, server) + 1, &ready, nullptr, nullptr,
                 &timeout) <= 0)
        return;
      if (FD_ISSET(client, &ready)) {
        if (!Packet(client, packet)) return;
        if (packet.size() >= 11 && packet[4] == 3 &&
            std::memcmp(packet.data() + packet.size() - 6, "COMMIT", 6) == 0)
          committing = true;
        if (!Send(server, packet)) return;
      }
      if (FD_ISSET(server, &ready)) {
        if (!Packet(server, packet)) return;
        if (committing) {
          dropped_.store(packet.size() > 4 && packet[4] == 0);
          return;
        }
        if (!Send(client, packet)) return;
      }
    }
  }
  int listener_ = -1;
  std::atomic<int> client_{-1}, server_{-1};
  std::uint16_t port_ = 0;
  std::atomic<bool> dropped_{false};
  std::thread worker_;
};

class MysqlArchiveStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* database = std::getenv("YQUANT_MYSQL_TEST_DATABASE");
    if (database == nullptr)
      GTEST_SKIP() << "MySQL integration database is unset";
    std::snprintf(options.database, sizeof(options.database), "%s", database);
    std::snprintf(options.user, sizeof(options.user), "%s",
                  std::getenv("YQUANT_MYSQL_TEST_USER") != nullptr
                      ? std::getenv("YQUANT_MYSQL_TEST_USER")
                      : "root");
    const char* port = std::getenv("YQUANT_MYSQL_TEST_PORT");
    options.port = port == nullptr ? 33306 : std::strtoul(port, nullptr, 10);
    std::snprintf(options.password_env, sizeof(options.password_env),
                  "YQUANT_MYSQL_TEST_PASSWORD");
    admin = mysql_init(nullptr);
    ASSERT_NE(admin, nullptr);
    const char* password = std::getenv(options.password_env);
    ASSERT_NE(password, nullptr);
    ASSERT_NE(mysql_real_connect(admin, options.host, options.user, password,
                                 options.database, options.port, nullptr, 0),
              nullptr)
        << mysql_error(admin);
    static std::atomic<std::uint64_t> sequence{1};
    ledger_hi = std::uint64_t(getpid());
    ledger_lo = sequence.fetch_add(1);
    char encoded[33];
    std::snprintf(encoded, sizeof(encoded), "%016llx%016llx",
                  static_cast<unsigned long long>(ledger_hi),
                  static_cast<unsigned long long>(ledger_lo));
    key = std::string("UNHEX('") + encoded + "')";
    ASSERT_TRUE(store.Connect(options).ok()) << store.error_detail();
    config.market_data = marketd::testing::ScopeConfig();
    config.trading.initial_account.cash_balance = 1000000000;
    std::snprintf(config.persistence.directory,
                  sizeof(config.persistence.directory), "/unused");
    std::snprintf(config.persistence.account_key,
                  sizeof(config.persistence.account_key), "mock/account");
    ASSERT_TRUE(market.Configure(config.market_data).ok());
    ASSERT_TRUE(source.Configure(config.trading.initial_account).ok());
    ASSERT_TRUE(source.SetMutationSink(&sink).ok());
    source.SetRunning(true);
    ASSERT_TRUE(projection.Configure(config).ok());
  }

  // Mirrors identity v2's explicit fields, independently of projection code.
  std::vector<std::byte> Identity(bool second = false,
                                  std::uint64_t first_client = 41) {
    std::vector<std::byte> bytes(32768);
    StateWriter out(bytes);
    out.U64(2);
    out.Text(config.persistence.account_key);
    out.U64(ledger_hi);
    out.U64(ledger_lo);
    out.U64(second ? 4 : 3);
    out.U64(1024);
    out.U64(8192);
    out.U64(config.market_data.instrument_count);
    for (std::size_t i = 0; i < config.market_data.instrument_count; ++i) {
      const auto& d = config.market_data.instruments[i].instrument;
      out.U64(d.id.value);
      out.U64(static_cast<unsigned>(d.venue));
      out.Text(d.symbol_view());
      out.U64(d.price_decimals);
      out.U64(static_cast<unsigned>(d.type));
      out.U64(d.price_tick);
      out.U64(d.quantity_step);
      out.U64(d.contract_multiplier);
      out.U64(d.margin_rate_ppm);
      out.U64(d.open_fee_per_lot);
      out.U64(d.close_today_fee_per_lot);
      out.U64(d.close_yesterday_fee_per_lot);
    }
    out.U64(static_cast<unsigned>(config.trading.mode));
    out.U64(config.trading.report_interval_ns);
    for (std::size_t i = 0; i < trading::kMaxClients; ++i) {
      const bool present = i == 0 || (second && i == 1);
      out.U64(present ? (i == 0 ? first_client : 42) : 0);
      out.U64(present ? (i == 0 ? 2 : 4) : 0);
      out.U64(present ? 1 : 0);
      if (present) {
        out.U64(config.market_data.instruments[0].instrument.id.value);
        out.U64(config.market_data.instruments[0].instrument.price_decimals);
      }
    }
    EXPECT_TRUE(out.ok());
    bytes.resize(out.size());
    return bytes;
  }

  std::vector<std::byte> Baseline(bool has_order = false,
                                  std::uint64_t run = 1) {
    std::vector<std::byte> checkpoint(8 * 1024 * 1024);
    StateWriter out(checkpoint);
    out.U64(1);
    out.U64(ledger_hi);
    out.U64(ledger_lo);
    out.U64(run);
    out.U64(0);
    out.U64(1);
    out.U64(64);
    out.U64(clock.Now());
    for (std::size_t i = 0; i < trading::kMaxClients; ++i) {
      out.U64(i == 0 ? 41 : 0);
      out.U64(i == 0 ? 2 : 0);
      out.U64(i == 0 && has_order ? 1 : 0);
      out.U64(0);
      if (i == 0 && has_order) {
        out.U64(1);
        out.U64(2);
        out.U64(0);
        out.U64(0);
        out.U64(0);
      }
    }
    std::vector<std::byte> ledger(trading::kMaxTradingCheckpointBytes);
    std::size_t size = 0;
    EXPECT_TRUE(source.ExportCheckpoint(ledger, size).ok());
    out.U64(size);
    out.Bytes(std::span(ledger).first(size));
    out.U64(0);  // Mock image is irrelevant to archive accounting.
    EXPECT_TRUE(out.ok());
    checkpoint.resize(out.size());
    const auto identity = Identity();
    std::vector<std::byte> bytes(checkpoint.size() + identity.size() + 256);
    StateWriter baseline(bytes);
    baseline.U64(1);
    baseline.U64(ledger_hi);
    baseline.U64(ledger_lo);
    baseline.U64(run);
    baseline.Text(config.persistence.account_key);
    baseline.U64(identity.size());
    baseline.Bytes(identity);
    baseline.U64(checkpoint.size());
    baseline.Bytes(checkpoint);
    EXPECT_TRUE(baseline.ok());
    bytes.resize(baseline.size());
    return bytes;
  }

  void SubmitAndFill() {
    source.BeginCallback(strategy::CallbackPhase::kTick);
    model::ClientOrderId id;
    ASSERT_EQ(
        source.SubmitOrder({config.market_data.instruments[0].instrument.id,
                            model::OrderSide::kBuy, 1000, 10},
                           id),
        ErrorCode::kOk);
    source.EndCallback();
    ASSERT_TRUE(mock.Prepare(config.trading).ok());
    ASSERT_TRUE(mock.Start(clock.Now()).ok());
    ASSERT_TRUE(source.ProcessRequests(mock, clock.Now()).ok());
    model::TradingEvent accepted;
    accepted.client_order_id = id;
    ASSERT_EQ(accepted.adapter_order_id.Assign("broker-1"), ErrorCode::kOk);
    accepted.report_sequence = 1;
    accepted.receive_time_ns = clock.Now();
    trading::TradingDispatch dispatch;
    ASSERT_EQ(source.ApplyEvent(accepted, dispatch), ErrorCode::kOk);
    auto fill = accepted;
    fill.report_sequence = 2;
    fill.kind = model::OrderUpdateKind::kFill;
    fill.cumulative_filled_qty = 4;
    fill.fill.client_order_id = id;
    fill.fill.adapter_order_id = accepted.adapter_order_id;
    ASSERT_EQ(fill.fill.execution_id.Assign("execution-1"), ErrorCode::kOk);
    fill.fill.instrument_id = config.market_data.instruments[0].instrument.id;
    fill.fill.side = model::OrderSide::kBuy;
    fill.fill.price = 999;
    fill.fill.quantity = 4;
    fill.fill.receive_time_ns = clock.Now();
    ASSERT_EQ(source.ApplyEvent(fill, dispatch), ErrorCode::kOk);
  }

  std::vector<std::byte> LedgerRecord(const trading::TradingMutation& mutation,
                                      std::size_t slot = 0,
                                      std::size_t local = 0,
                                      std::uint64_t cancel = 0) {
    std::vector<std::byte> bytes(2048);
    StateWriter writer(bytes);
    writer.U64(slot);
    writer.U64(local);
    writer.U64(cancel);
    std::size_t size = 0;
    EXPECT_TRUE(
        trading::EncodeTradingMutation(mutation, writer.tail(), size).ok());
    writer.Advance(size);
    bytes.resize(writer.size());
    return bytes;
  }

  std::vector<std::byte> RejectionRecord(const trading::Message& message,
                                         std::size_t local = 0) {
    std::vector<std::byte> bytes(8192);
    StateWriter writer(bytes);
    writer.U64(0);
    writer.U64(local);
    std::array<std::byte, trading::kMaxFrameSize> encoded{};
    std::size_t size = 0;
    EXPECT_TRUE(trading::EncodeMessage(message, encoded, size).ok());
    writer.U64(size);
    writer.Bytes(std::span(encoded).first(size));
    bytes.resize(writer.size());
    return bytes;
  }

  void TearDown() override {
    store.Close();
    if (admin == nullptr) return;
    for (const char* table :
         {"archive_progress", "archive_runs", "archive_gaps",
          "strategy_sessions", "trade_events", "orders", "fills",
          "order_updates", "account_cash", "account_positions", "ledgers"}) {
      const auto sql =
          std::string("DELETE FROM ") + table + " WHERE ledger_id=" + key;
      EXPECT_EQ(mysql_query(admin, sql.c_str()), 0) << mysql_error(admin);
    }
    mysql_close(admin);
  }
  std::uint64_t Scalar(std::string sql) {
    sql += " WHERE ledger_id=" + key;
    EXPECT_EQ(mysql_query(admin, sql.c_str()), 0) << mysql_error(admin);
    MYSQL_RES* result = mysql_store_result(admin);
    if (result == nullptr) return UINT64_MAX;
    MYSQL_ROW row = mysql_fetch_row(result);
    const auto value = row == nullptr || row[0] == nullptr
                           ? UINT64_MAX
                           : std::strtoull(row[0], nullptr, 10);
    mysql_free_result(result);
    return value;
  }
  void Install() {
    baseline = Baseline();
    ASSERT_TRUE(projection.Restore(baseline).ok());
    MysqlArchiveProgress progress;
    ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
    ASSERT_FALSE(progress.exists);
    ASSERT_TRUE(store.InstallRun(baseline, projection, 0, 0, false).ok())
        << store.error_detail();
    ASSERT_TRUE(store.Commit({1, 1, 64, 0}, projection).ok())
        << store.error_detail();
  }
  void Consume() {
    MysqlArchiveProgress progress;
    ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
    std::uint64_t lsn = 0;
    for (const auto& fact : sink.facts) {
      const auto record = LedgerRecord(fact);
      ASSERT_TRUE(projection.Replay(1, ++lsn, record).ok());
      ASSERT_TRUE(store.AppendEvent(1, lsn, record, projection).ok())
          << store.error_detail();
    }
    ASSERT_TRUE(store.Commit({1, 1, 128, lsn}, projection).ok())
        << store.error_detail();
  }
  MysqlArchiveOptions options;
  MysqlArchiveStore store;
  MYSQL* admin = nullptr;
  std::uint64_t ledger_hi = 0, ledger_lo = 0;
  std::string key;
  std::vector<std::byte> baseline;

  ArchiveClock clock;
  ServerConfig config;
  md::InstrumentScope market;
  ::yquant::testing::TradingEngineHarness source{clock, market};
  trading::MockTradingAdapter mock;
  FactSink sink;
  ArchiveProjection projection{clock};
};

TEST_F(MysqlArchiveStoreTest, PublishesFactsProjectionAndWatermarkAtomically) {
  Install();
  SubmitAndFill();
  Consume();
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM trade_events"), sink.facts.size());
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM fills"), 1);
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM orders"), 1);
  EXPECT_EQ(Scalar("SELECT cumulative_filled_qty FROM orders"), 4);
  EXPECT_EQ(Scalar("SELECT balance FROM account_cash"), 960040000U);
  EXPECT_EQ(Scalar("SELECT lsn FROM archive_progress"), sink.facts.size());
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM account_positions"),
            config.market_data.instrument_count);
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
  ASSERT_TRUE(store.FinalizeRun(projection, progress.cursor.lsn).ok())
      << store.error_detail();
  ASSERT_TRUE(store.Commit(progress.cursor, projection).ok())
      << store.error_detail();
  EXPECT_EQ(Scalar("SELECT final_lsn FROM archive_runs"), sink.facts.size());
}

TEST_F(MysqlArchiveStoreTest, RollbackLeavesFactsAndProgressAtPreviousCommit) {
  Install();
  SubmitAndFill();
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
  auto record = LedgerRecord(sink.facts[0]);
  ASSERT_TRUE(projection.Replay(1, 1, record).ok());
  ASSERT_TRUE(store.AppendEvent(1, 1, record, projection).ok())
      << store.error_detail();
  store.Rollback();
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM trade_events"), 0);
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM orders"), 0);
  EXPECT_EQ(Scalar("SELECT lsn FROM archive_progress"), 0);
}

TEST_F(MysqlArchiveStoreTest, RejectsConflictingEventWithoutAdvancingProgress) {
  Install();
  const auto sql =
      "INSERT INTO trade_events "
      "(ledger_id,run_id,lsn,record_type,encoding_version,payload,"
      "monotonic_time_ns) VALUES (" +
      key + ",1,1,1,1,X'00',1000)";
  ASSERT_EQ(mysql_query(admin, sql.c_str()), 0) << mysql_error(admin);
  SubmitAndFill();
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
  auto record = LedgerRecord(sink.facts[0]);
  ASSERT_TRUE(projection.Replay(1, 1, record).ok());
  EXPECT_FALSE(store.AppendEvent(1, 1, record, projection).ok());
  EXPECT_FALSE(store.retriable());
  store.Rollback();
  EXPECT_EQ(Scalar("SELECT lsn FROM archive_progress"), 0);
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM orders"), 0);
}

TEST_F(MysqlArchiveStoreTest, CompetingWriterReadsCommittedProgressAfterLock) {
  Install();
  SubmitAndFill();
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
  auto competing = std::async(std::launch::async, [this] {
    MysqlArchiveStore other;
    auto status = other.Connect(options);
    if (!status.ok()) return false;
    MysqlArchiveProgress locked;
    status = other.Begin(projection, locked);
    const bool matches = status.ok() && locked.cursor.lsn == sink.facts.size();
    other.Rollback();
    return matches;
  });
  EXPECT_EQ(competing.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  std::uint64_t lsn = 0;
  for (const auto& fact : sink.facts) {
    const auto record = LedgerRecord(fact);
    ASSERT_TRUE(projection.Replay(1, ++lsn, record).ok());
    ASSERT_TRUE(store.AppendEvent(1, lsn, record, projection).ok())
        << store.error_detail();
  }
  ASSERT_TRUE(store.Commit({1, 1, 128, lsn}, projection).ok())
      << store.error_detail();
  EXPECT_TRUE(competing.get());
}

TEST_F(MysqlArchiveStoreTest, WaitingWriterSeesNewRunInstalledAfterBegin) {
  Install();
  ASSERT_EQ(mysql_query(admin, "START TRANSACTION"), 0);
  const auto lock_sql =
      "SELECT run_id FROM archive_progress WHERE ledger_id=" + key +
      " FOR UPDATE";
  ASSERT_EQ(mysql_query(admin, lock_sql.c_str()), 0) << mysql_error(admin);
  MYSQL_RES* locked = mysql_store_result(admin);
  ASSERT_NE(locked, nullptr);
  mysql_free_result(locked);
  // Wait until the other writer has completed its ledger identity lookup and
  // reached progress insertion. Its first RR snapshot used to predate the
  // concurrent run installation below.
  std::atomic<unsigned> statements{0};
  auto competing = std::async(std::launch::async, [this, &statements] {
    MysqlArchiveStore other;
    auto status = other.Connect(options);
    if (!status.ok()) return false;
    other.SetCancellation(
        [](void* context) noexcept {
          static_cast<std::atomic<unsigned>*>(context)->fetch_add(1);
          return false;
        },
        &statements);
    MysqlArchiveProgress progress;
    status = other.Begin(projection, progress);
    const bool current = status.ok() && progress.cursor.stream_id == 3;
    other.Rollback();
    return current;
  });
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (statements.load() < 4 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_GE(statements.load(), 4);
  const auto new_baseline = Baseline(false, 3);
  std::string encoded;
  constexpr char kHex[] = "0123456789abcdef";
  for (const auto byte : new_baseline) {
    const auto value = std::to_integer<unsigned>(byte);
    encoded.push_back(kHex[value >> 4]);
    encoded.push_back(kHex[value & 15]);
  }
  const auto install_sql =
      "INSERT INTO archive_runs (ledger_id,run_id,previous_run_id,"
      "previous_end_lsn,baseline_version,baseline,history_gap) VALUES (" +
      key + ",3,1,0,1,X'" + encoded + "',0)";
  EXPECT_EQ(mysql_query(admin, install_sql.c_str()), 0) << mysql_error(admin);
  const auto progress_sql =
      "UPDATE archive_progress SET run_id=3 WHERE ledger_id=" + key;
  EXPECT_EQ(mysql_query(admin, progress_sql.c_str()), 0) << mysql_error(admin);
  EXPECT_EQ(mysql_query(admin, "COMMIT"), 0) << mysql_error(admin);
  EXPECT_TRUE(competing.get());
}

TEST_F(MysqlArchiveStoreTest, CancellationRollsBackUnpublishedEvents) {
  Install();
  SubmitAndFill();
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok());
  auto record = LedgerRecord(sink.facts[0]);
  ASSERT_TRUE(projection.Replay(1, 1, record).ok());
  ASSERT_TRUE(store.AppendEvent(1, 1, record, projection).ok());
  store.SetCancellation([](void*) noexcept { return true; }, nullptr);
  EXPECT_FALSE(store.Commit({1, 1, 128, 1}, projection).ok());
  EXPECT_TRUE(store.retriable());
  store.Rollback();
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM trade_events"), 0);
  EXPECT_EQ(Scalar("SELECT lsn FROM archive_progress"), 0);
}

TEST_F(MysqlArchiveStoreTest,
       LostCommitReplyResumesFromActuallyCommittedWatermark) {
  if (std::strlen(std::getenv(options.password_env)) != 0)
    GTEST_SKIP() << "the plaintext test relay requires an empty test password";
  Install();
  SubmitAndFill();
  store.Close();
  CommitReplyProxy proxy(options.port);
  ASSERT_NE(proxy.port(), 0);
  auto proxied = options;
  proxied.port = proxy.port();
  ASSERT_TRUE(store.Connect(proxied).ok()) << store.error_detail();
  MysqlArchiveProgress progress;
  ASSERT_TRUE(store.Begin(projection, progress).ok()) << store.error_detail();
  std::uint64_t lsn = 0;
  for (const auto& fact : sink.facts) {
    const auto record = LedgerRecord(fact);
    ASSERT_TRUE(projection.Replay(1, ++lsn, record).ok());
    ASSERT_TRUE(store.AppendEvent(1, lsn, record, projection).ok())
        << store.error_detail();
  }
  EXPECT_FALSE(store.Commit({1, 1, 128, lsn}, projection).ok());
  EXPECT_TRUE(store.retriable());
  EXPECT_TRUE(proxy.dropped_commit_ok());
  EXPECT_EQ(Scalar("SELECT lsn FROM archive_progress"), lsn);
  EXPECT_EQ(Scalar("SELECT COUNT(*) FROM fills"), 1);
  ASSERT_TRUE(store.Connect(options).ok()) << store.error_detail();
  ASSERT_TRUE(store.GetProgress(projection, progress).ok())
      << store.error_detail();
  EXPECT_EQ(progress.cursor.lsn, lsn);
  EXPECT_EQ(Scalar("SELECT balance FROM account_cash"), 960040000U);
}

}  // namespace
}  // namespace yquant::tradingd
#endif
