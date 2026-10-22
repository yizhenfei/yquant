#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "marketd/csv_replay.h"
#include "marketd/recording.h"
#include "scoped_working_directory.h"
#include "test_support.h"
#include "trading_gateway.h"
#include "yquant/md/quote_codec.h"
#include "yquant/md/shm_broadcast.h"

extern char** environ;

namespace yquant::marketd {
namespace {

constexpr auto kProcessTimeout = std::chrono::seconds(10);
// Parsing and recording 367171 quotes needs a larger bounded budget under
// sanitizers and on shared CI hosts; small process tests retain their limit.
constexpr auto kLargeHistoryProcessTimeout = std::chrono::seconds(60);

// Owns one real daemon and a nonblocking combined output pipe. Failed tests
// kill and reap their children, so no failure leaves a feed or writer lock
// live.
class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess() {
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, &status_, 0) == -1 && errno == EINTR) {
      }
    }
    if (output_fd_ >= 0) close(output_fd_);
  }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  int Start(const char* executable, const std::vector<std::string>& arguments) {
    auto adapted = arguments;
    if (std::string_view(executable) == YQUANT_TEST_STRATEGYD_PATH) {
      const int error = gateway_.Prepare(adapted);
      if (error) return error;
    }
    int output[2];
    if (pipe(output) != 0) return errno;
    std::vector<std::string> storage{executable};
    storage.insert(storage.end(), adapted.begin(), adapted.end());
    std::vector<char*> argv;
    for (auto& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error == 0) {
      error =
          posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
      if (error == 0)
        error = posix_spawn_file_actions_adddup2(&actions, output[1],
                                                 STDERR_FILENO);
      if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, output[0]);
      if (error == 0)
        error = posix_spawn_file_actions_addclose(&actions, output[1]);
      if (error == 0)
        error = posix_spawn(&pid_, executable, &actions, nullptr, argv.data(),
                            environ);
      posix_spawn_file_actions_destroy(&actions);
    }
    close(output[1]);
    if (error != 0) {
      close(output[0]);
      pid_ = -1;
      return error;
    }
    output_fd_ = output[0];
    if (fcntl(output_fd_, F_SETFL, O_NONBLOCK) != 0) return errno;
    if (fcntl(output_fd_, F_SETFD, FD_CLOEXEC) != 0) return errno;
    return 0;
  }

  bool WaitForOutput(std::string_view text) {
    return ObserveUntil(
        [&] { return output_.find(text) != std::string::npos; });
  }
  bool Wait(std::chrono::milliseconds timeout = kProcessTimeout) {
    return ObserveUntil([&] { return pid_ < 0; }, timeout);
  }
  bool Signal(int signal) const { return pid_ > 0 && kill(pid_, signal) == 0; }
  int exit_code() const {
    return exited_ && WIFEXITED(status_) ? WEXITSTATUS(status_) : -1;
  }
  const std::string& output() const { return output_; }

 private:
  // Reads only currently available bytes and retries interrupted reads.
  void Drain() {
    char buffer[1024];
    for (;;) {
      const auto count = read(output_fd_, buffer, sizeof(buffer));
      if (count > 0)
        output_.append(buffer, static_cast<std::size_t>(count));
      else if (count < 0 && errno == EINTR)
        continue;
      else
        return;
    }
  }

  // Observes process exit and output under a single deadline per expectation.
  template <typename Predicate>
  bool ObserveUntil(Predicate done,
                    std::chrono::milliseconds timeout = kProcessTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      Drain();
      if (pid_ > 0) {
        const auto result = waitpid(pid_, &status_, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          exited_ = true;
          Drain();
        } else if (result < 0 && errno != EINTR) {
          return false;
        }
      }
      if (done()) return true;
      if (pid_ < 0) return false;
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (remaining.count() <= 0) return false;
      pollfd descriptor{output_fd_, POLLIN, 0};
      if (poll(&descriptor, 1, static_cast<int>(remaining.count())) < 0 &&
          errno != EINTR)
        return false;
    }
  }

  ::yquant::testing::TradingGateway gateway_;
  pid_t pid_ = -1;
  int output_fd_ = -1;
  int status_ = 0;
  bool exited_ = false;
  std::string output_;
};

// Reads a live IPC stream without silently skipping gaps or malformed events.
bool ReadQuote(md::ShmReader& reader, model::QuoteTick& out) {
  const auto deadline = std::chrono::steady_clock::now() + kProcessTimeout;
  md::EventBuffer buffer;
  for (;;) {
    const auto result = reader.TryNext(buffer);
    if (result.status == md::ReadStatus::kEvent)
      return md::DecodeQuoteEvent(buffer.view(), out).ok();
    if (result.status != md::ReadStatus::kEmpty &&
        result.status != md::ReadStatus::kRetry)
      return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// Reserves a kernel-selected local TCP port until this setup call returns.
std::uint16_t AvailablePort() {
  const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (descriptor < 0) return 0;
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t size = sizeof(address);
  const bool success =
      bind(descriptor, reinterpret_cast<sockaddr*>(&address), size) == 0 &&
      getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) ==
          0;
  close(descriptor);
  return success ? ntohs(address.sin_port) : 0;
}

class MarketdIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.valid());
    ASSERT_TRUE(working_directory_.valid());
    ASSERT_TRUE(directory_.Write("mock.json", testing::kMockJson));
    ASSERT_TRUE(directory_.Write("local.json", LocalJson()));
    ASSERT_TRUE(directory_.Write(
        "strategy.json", R"({"strategy_id":"marketd-observer","parameters":{)"
                         R"("venue":"SSE","symbol":"600000"}})"));
    ASSERT_TRUE(directory_.Write(
        "strategy_host.json",
        R"({"source_id":42,"adapter":"shm",)"
        R"("instrument_catalog":"instruments.json","runtime_directory":")" +
            std::string(directory_.path()) +
            R"(","stream":"quotes",)"
            R"("poll_interval_us":0,"instruments":[{"venue":"SSE",)"
            R"("symbol":"600000"}]})"));
  }

  std::string ShmJson() const {
    return R"({"adapter":"shm","runtime_directory":")" +
           std::string(directory_.path()) +
           R"(","stream":"quotes","capacity":8192,"max_payload_size":80})";
  }
  std::string LocalJson() const {
    return R"({"source":{"adapter":"mock","config":"mock.json"},)"
           R"("publishers":[)" +
           ShmJson() + "]}";
  }
  int StartMarketd(ChildProcess& child, std::string_view config = "local.json",
                   const std::vector<std::string>& extra = {}) const {
    std::vector<std::string> arguments{"--config", directory_.File(config)};
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    return child.Start(YQUANT_TEST_MARKETD_PATH, arguments);
  }
  int StartStrategy(ChildProcess& child) const {
    return child.Start(
        YQUANT_TEST_STRATEGYD_PATH,
        {"--strategy", YQUANT_TEST_QUOTE_EXAMPLE_PATH, "--config",
         directory_.File("strategy.json"), "--market-data-config",
         directory_.File("strategy_host.json"), "--trading-config",
         YQUANT_TEST_TRADING_CONFIG_PATH});
  }
  void Stop(ChildProcess& child) const {
    ASSERT_TRUE(child.Signal(SIGTERM)) << child.output();
    ASSERT_TRUE(child.Wait()) << child.output();
    EXPECT_EQ(child.exit_code(), 0) << child.output();
  }

  testing::RuntimeDirectory directory_;
  ::yquant::testing::ScopedWorkingDirectory working_directory_{
      directory_.path()};
};

TEST_F(MarketdIntegrationTest, TwoStrategiesAndRawReadersObserveBroadcast) {
  ChildProcess marketd;
  ASSERT_EQ(StartMarketd(marketd), 0);
  ASSERT_TRUE(marketd.WaitForOutput("Running")) << marketd.output();
  md::ShmReader first_reader;
  md::ShmReader second_reader;
  const md::Endpoint endpoint{directory_.path(), "quotes"};
  ASSERT_TRUE(first_reader.Open(endpoint, md::StartPosition::At(1)).ok());
  ASSERT_TRUE(second_reader.Open(endpoint, md::StartPosition::At(1)).ok());
  ChildProcess first_strategy;
  ChildProcess second_strategy;
  ASSERT_EQ(StartStrategy(first_strategy), 0);
  ASSERT_EQ(StartStrategy(second_strategy), 0);
  ASSERT_TRUE(first_strategy.WaitForOutput("first quote received"))
      << first_strategy.output();
  ASSERT_TRUE(second_strategy.WaitForOutput("first quote received"))
      << second_strategy.output();
  for (std::uint64_t i = 1; i <= 16; ++i) {
    model::QuoteTick first;
    model::QuoteTick second;
    ASSERT_TRUE(ReadQuote(first_reader, first));
    ASSERT_TRUE(ReadQuote(second_reader, second));
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.sequence, i);
    EXPECT_EQ(first.source_id.value, 42U);
    EXPECT_EQ(first.receive_time_ns, 0U);
  }
  Stop(first_strategy);
  Stop(second_strategy);
  EXPECT_NE(first_strategy.output().find("ticks="), std::string::npos);
  EXPECT_NE(second_strategy.output().find("ticks="), std::string::npos);
  Stop(marketd);
  md::ShmReader later;
  EXPECT_FALSE(later.Open(endpoint).ok());
}

TEST_F(MarketdIntegrationTest, ShmQuotesDriveTradingAndAccountSettlement) {
  // Stable prices make final cash independent of when the SHM reader joins.
  ASSERT_TRUE(directory_.Write(
      "order_mock.json",
      R"({"source_id":1,"adapter":"mock","interval_ms":1,)"
      R"("instrument_catalog":"instruments.json","instruments":[)"
      R"({"venue":"SSE","symbol":"600000",)"
      R"("bid_price":1000,"bid_qty":100,"ask_price":1001,"ask_qty":200,)"
      R"("price_step":0}]})"));
  ASSERT_TRUE(directory_.Write(
      "order_feed.json",
      R"({"source":{"adapter":"mock","config":"order_mock.json"},)"
      R"("publishers":[)" +
          ShmJson() + "]}"));
  ASSERT_TRUE(directory_.Write(
      "order_host.json",
      R"({"source_id":1,"adapter":"shm",)"
      R"("instrument_catalog":"instruments.json","runtime_directory":")" +
          std::string(directory_.path()) +
          R"(","stream":"quotes","poll_interval_us":0,"instruments":[)"
          R"({"venue":"SSE","symbol":"600000"}]})"));
  ChildProcess marketd;
  ASSERT_EQ(StartMarketd(marketd, "order_feed.json"), 0);
  ASSERT_TRUE(marketd.WaitForOutput("Running")) << marketd.output();
  ChildProcess strategy;
  ASSERT_EQ(strategy.Start(
                YQUANT_TEST_STRATEGYD_PATH,
                {"--strategy", YQUANT_TEST_ORDER_EXAMPLE_PATH, "--config",
                 YQUANT_TEST_ORDER_EXAMPLE_CONFIG_PATH, "--market-data-config",
                 directory_.File("order_host.json"), "--trading-config",
                 YQUANT_TEST_TRADING_CONFIG_PATH}),
            0);
  ASSERT_TRUE(strategy.WaitForOutput(
      "order_lifecycle: terminal state=filled cumulative=200 leaves=0\n"))
      << strategy.output();
  EXPECT_NE(
      strategy.output().find(
          "order_lifecycle: account initial cash=100000000000000 reserved=0 "
          "available=100000000000000 position_total=1000 "
          "position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
  Stop(strategy);
  EXPECT_NE(
      strategy.output().find(
          "order_lifecycle: stopped fills=2 filled_qty=200 state=filled\n"),
      std::string::npos);
  EXPECT_NE(
      strategy.output().find(
          "order_lifecycle: account final cash=99997998000000 reserved=0 "
          "available=99997998000000 position_total=1200 position_available=800 "
          "position_reserved=0\n"),
      std::string::npos);
  Stop(marketd);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.manifest")));
}

TEST_F(MarketdIntegrationTest, DuplicatePublisherFailsWhileOriginalStaysLive) {
  ChildProcess first;
  ASSERT_EQ(StartMarketd(first), 0);
  ASSERT_TRUE(first.WaitForOutput("Running")) << first.output();
  ChildProcess second;
  ASSERT_EQ(StartMarketd(second), 0);
  ASSERT_TRUE(second.Wait()) << second.output();
  EXPECT_NE(second.exit_code(), 0) << second.output();
  EXPECT_NE(second.output().find("publisher_init"), std::string::npos);
  md::ShmReader reader;
  ASSERT_TRUE(reader.Open({directory_.path(), "quotes"}).ok());
  model::QuoteTick quote;
  EXPECT_TRUE(ReadQuote(reader, quote));
  Stop(first);
}

TEST_F(MarketdIntegrationTest, CheckConfigCreatesNoDiscoveryOrWriterLock) {
  ChildProcess child;
  ASSERT_EQ(StartMarketd(child, "local.json", {"--check-config"}), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find("configuration valid"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.manifest")));
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.lock")));
}

TEST_F(MarketdIntegrationTest, EventLimitExitsAndRemovesDiscovery) {
  ChildProcess child;
  ASSERT_EQ(StartMarketd(child, "local.json", {"--max-events", "17"}), 0);
  ASSERT_TRUE(child.Wait()) << child.output();
  EXPECT_EQ(child.exit_code(), 0) << child.output();
  EXPECT_NE(child.output().find("Stopped events=17"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.manifest")));
  md::ShmPublisher publisher;
  EXPECT_TRUE(publisher.Create({{directory_.path(), "quotes"}, 16, 80}).ok());
}

TEST_F(MarketdIntegrationTest, CrashRestartChangesGenerationAndReleasesLock) {
  ChildProcess first;
  ASSERT_EQ(StartMarketd(first), 0);
  ASSERT_TRUE(first.WaitForOutput("Running")) << first.output();
  md::ShmReader old_reader;
  ASSERT_TRUE(old_reader.Open({directory_.path(), "quotes"}).ok());
  const auto generation = old_reader.generation();
  ASSERT_TRUE(first.Signal(SIGKILL));
  ASSERT_TRUE(first.Wait()) << first.output();
  ChildProcess replacement;
  ASSERT_EQ(StartMarketd(replacement), 0);
  ASSERT_TRUE(replacement.WaitForOutput("Running")) << replacement.output();
  md::ShmReader new_reader;
  ASSERT_TRUE(new_reader.Open({directory_.path(), "quotes"}).ok());
  EXPECT_NE(new_reader.generation(), generation);
  md::RefreshResult result;
  ASSERT_TRUE(old_reader.Refresh(result).ok());
  EXPECT_EQ(result.status, md::RefreshStatus::kStreamChanged);
  model::QuoteTick quote;
  EXPECT_TRUE(ReadQuote(new_reader, quote));
  Stop(replacement);
}

TEST_F(MarketdIntegrationTest,
       TcpRelayFeedsRealStrategyAndPreservesQuoteOrigin) {
  const auto port = AvailablePort();
  ASSERT_NE(port, 0);
  ASSERT_TRUE(directory_.Write(
      "server.json",
      R"({"source":{"adapter":"mock","config":"mock.json"},)"
      R"("publishers":[{"adapter":"tcp","address":"127.0.0.1","port":)" +
          std::to_string(port) + "}]}"));
  // Keep listener and client addresses explicit; the receiver is an ordinary
  // configured source and has no deployment-role flag.
  ASSERT_TRUE(directory_.Write(
      "relay.json",
      R"({"source":{"adapter":"tcp","address":"127.0.0.1","port":)" +
          std::to_string(port) + "},\"publishers\":[" + ShmJson() + "]}"));
  ChildProcess server;
  ASSERT_EQ(StartMarketd(server, "server.json"), 0);
  ASSERT_TRUE(server.WaitForOutput("Running")) << server.output();
  ChildProcess relay;
  ASSERT_EQ(StartMarketd(relay, "relay.json"), 0);
  ASSERT_TRUE(relay.WaitForOutput("Running")) << relay.output();
  md::ShmReader reader;
  ASSERT_TRUE(
      reader.Open({directory_.path(), "quotes"}, md::StartPosition::At(1))
          .ok());
  ChildProcess strategy;
  ASSERT_EQ(StartStrategy(strategy), 0);
  ASSERT_TRUE(strategy.WaitForOutput("first quote received"))
      << strategy.output();
  model::QuoteTick previous;
  ASSERT_TRUE(ReadQuote(reader, previous));
  EXPECT_EQ(previous.source_id.value, 42U);
  EXPECT_GT(previous.event_time_ns, 0U);
  EXPECT_EQ(previous.receive_time_ns, 0U);
  for (int i = 0; i < 15; ++i) {
    model::QuoteTick current;
    ASSERT_TRUE(ReadQuote(reader, current));
    EXPECT_EQ(current.source_id, previous.source_id);
    EXPECT_EQ(current.instrument_id, previous.instrument_id);
    EXPECT_EQ(current.sequence, previous.sequence + 1);
    EXPECT_GE(current.event_time_ns, previous.event_time_ns);
    previous = current;
  }
  Stop(strategy);
  Stop(relay);
  Stop(server);
}

TEST_F(MarketdIntegrationTest, HelpAndInvalidArgumentsHaveDeterministicExit) {
  ChildProcess help;
  ASSERT_EQ(help.Start(YQUANT_TEST_MARKETD_PATH, {"--help"}), 0);
  ASSERT_TRUE(help.Wait());
  EXPECT_EQ(help.exit_code(), 0);
  EXPECT_NE(help.output().find("Usage:"), std::string::npos);
  ChildProcess invalid;
  ASSERT_EQ(StartMarketd(invalid, "local.json", {"--max-events", "0"}), 0);
  ASSERT_TRUE(invalid.Wait());
  EXPECT_EQ(invalid.exit_code(), 2);
  EXPECT_FALSE(std::filesystem::exists(directory_.File("quotes.manifest")));
}

TEST_F(MarketdIntegrationTest, RealHistoryRecordsAndReplaysEveryQuote) {
  ASSERT_TRUE(directory_.Copy("instruments.json",
                              YQUANT_TEST_EXAMPLE_INSTRUMENT_CATALOG_PATH));
  const char* data_directory = std::getenv("YQUANT_TEST_DATA_DIR");
  ASSERT_NE(data_directory, nullptr);
  const std::string csv_path =
      std::string(data_directory) + "/dolphindb/2020-12-31/snapshot.csv";
  ASSERT_TRUE(directory_.Write(
      "history.json",
      R"({"source":{"adapter":"replay","format":"dolphindb_csv",)"
      R"("path":")" +
          csv_path +
          R"(","source_id":7,"instrument_catalog":"instruments.json",)"
          R"("speed":0,"max_records":400000},"publishers":[)"
          R"({"adapter":"record","path":"first.yqr","queue_capacity":67108864}]})"));
  ChildProcess first;
  ASSERT_EQ(StartMarketd(first, "history.json"), 0);
  ASSERT_TRUE(first.Wait(kLargeHistoryProcessTimeout)) << first.output();
  ASSERT_EQ(first.exit_code(), 0) << first.output();
  EXPECT_NE(first.output().find("Stopped events=367171"), std::string::npos);

  ASSERT_TRUE(directory_.Write(
      "recording.json",
      R"({"source":{"adapter":"replay","format":"recording",)"
      R"("path":"first.yqr","speed":0,"max_records":400000},)"
      R"("publishers":[{"adapter":"record","path":"second.yqr",)"
      R"("queue_capacity":67108864}]})"));
  ChildProcess second;
  ASSERT_EQ(StartMarketd(second, "recording.json"), 0);
  ASSERT_TRUE(second.Wait(kLargeHistoryProcessTimeout)) << second.output();
  ASSERT_EQ(second.exit_code(), 0) << second.output();
  EXPECT_NE(second.output().find("Stopped events=367171"), std::string::npos);

  ReplayConfig csv;
  std::snprintf(csv.path, sizeof(csv.path), "%s", csv_path.c_str());
  csv.format = ReplayFormat::kDolphinDbCsv;
  csv.source_id = {7};
  std::snprintf(csv.instrument_catalog, sizeof(csv.instrument_catalog), "%s",
                directory_.File("instruments.json").c_str());
  csv.max_records = 400000;
  ReplayData original;
  ReplayData recorded;
  ReplayData replayed;
  char detail[256];
  ASSERT_TRUE(LoadDolphinDbCsv(csv, original, detail).ok()) << detail;
  ASSERT_TRUE(LoadRecordingFile(directory_.File("first.yqr").c_str(), 400000,
                                recorded, detail)
                  .ok())
      << detail;
  ASSERT_TRUE(LoadRecordingFile(directory_.File("second.yqr").c_str(), 400000,
                                replayed, detail)
                  .ok())
      << detail;
  ASSERT_EQ(original.size(), 367171U);
  ASSERT_EQ(recorded.size(), original.size());
  ASSERT_EQ(replayed.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(recorded[i].quote, original[i].quote) << "record " << i;
    EXPECT_EQ(replayed[i].quote, original[i].quote) << "replay " << i;
    if (::testing::Test::HasFailure()) break;
  }
}

TEST_F(MarketdIntegrationTest, ReplayFeedsSharedMemoryWhileRecording) {
  ASSERT_TRUE(directory_.Write(
      "short.csv",
      "Date,Time,SecurityID,SecurityIDSource,BidPX1,BidSize1,OfferPX1,"
      "OfferSize1\n"
      "2020.12.31,09:15:00.000,600000,101,10,100,10.01,200\n"
      "2020.12.31,09:15:00.020,600000,101,10.01,100,10.02,200\n"));
  ASSERT_TRUE(directory_.Write(
      "replay.json",
      R"({"source":{"adapter":"replay","format":"dolphindb_csv",)"
      R"("path":"short.csv","source_id":42,)"
      R"("instrument_catalog":"instruments.json",)"
      R"("start_delay_ms":500},"publishers":[)" +
          ShmJson() + R"(,{"adapter":"record","path":"short.yqr"}]})"));
  ChildProcess marketd;
  ASSERT_EQ(StartMarketd(marketd, "replay.json"), 0);
  ASSERT_TRUE(marketd.WaitForOutput("Running")) << marketd.output();
  md::ShmReader reader;
  ASSERT_TRUE(reader.Open({directory_.path(), "quotes"}).ok());
  model::QuoteTick quote;
  for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
    ASSERT_TRUE(ReadQuote(reader, quote));
    EXPECT_EQ(quote.sequence, sequence);
    EXPECT_EQ(quote.source_id.value, 42U);
    EXPECT_EQ(quote.bid_price, 999 + static_cast<std::int64_t>(sequence));
  }
  ASSERT_TRUE(marketd.Wait()) << marketd.output();
  EXPECT_EQ(marketd.exit_code(), 0) << marketd.output();
  ReplayData recording;
  char detail[256];
  ASSERT_TRUE(LoadRecordingFile(directory_.File("short.yqr").c_str(), 2,
                                recording, detail)
                  .ok())
      << detail;
  EXPECT_EQ(recording.size(), 2U);
}

}  // namespace
}  // namespace yquant::marketd
