#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>

#include "gtest/gtest.h"
#include "runner_test_harness.h"
#include "strategyd/market_data_source.h"
#include "test_config_file.h"
#include "yquant/base/event_loop_environment.h"
#include "yquant/trading/mock_trading_adapter.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;

class TradingEventPipe {
 public:
  TradingEventPipe() {
    const char* previous = std::getenv("YQUANT_TEST_EVENT_FD");
    had_previous_ = previous != nullptr;
    if (had_previous_) previous_ = previous;
    if (pipe(fds_) != 0 || fcntl(fds_[0], F_SETFL, O_NONBLOCK) == -1) return;
    const auto fd = std::to_string(fds_[1]);
    valid_ = setenv("YQUANT_TEST_EVENT_FD", fd.c_str(), 1) == 0;
  }
  ~TradingEventPipe() {
    if (had_previous_)
      setenv("YQUANT_TEST_EVENT_FD", previous_.c_str(), 1);
    else
      unsetenv("YQUANT_TEST_EVENT_FD");
    for (int fd : fds_)
      if (fd >= 0) close(fd);
  }
  bool valid() const { return valid_; }
  std::string Read() {
    std::string out;
    char buffer[256];
    for (;;) {
      const ssize_t count = read(fds_[0], buffer, sizeof(buffer));
      if (count > 0)
        out.append(buffer, static_cast<std::size_t>(count));
      else if (count == -1 && errno == EINTR)
        continue;
      else
        return out;
    }
  }

 private:
  int fds_[2] = {-1, -1};
  bool valid_ = false;
  bool had_previous_ = false;
  std::string previous_;
};

class TradingClock final : public base::MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override { return now; }
  std::uint64_t now = 0;
};

// Simulates time advancing inside a nonblocking poll. A future deadline can
// already be due by return, but must be judged against that poll's input time.
struct IdleClockStep {
  TradingClock* clock = nullptr;
  bool retain_due_deadline = false;
  int idle_returns = 0;
  std::optional<std::uint64_t> deadline;

  bool Poll(std::uint64_t now) noexcept {
    if (clock == nullptr || idle_returns != 0) {
      deadline.reset();
      return false;
    }
    ++idle_returns;
    deadline = now + (retain_due_deadline ? 0 : 1);
    clock->now = now + 1;
    return true;
  }
};

// Emits one quote immediately, then advertises a distant quote deadline. This
// isolates trading progress from future market-data wakeups without sleeping.
class TradingQuoteSource final : public MarketDataSource {
 public:
  int poll_calls = 0;
  int idle_polls = 0;
  int stop_calls = 0;
  bool continuous = false;
  bool poll_driven_idle = false;
  bool fail_service = false;
  int due_services = 0;
  std::uint64_t serviced_at = 0;
  std::optional<std::uint64_t> service_deadline;
  IdleClockStep idle;

  base::Status Prepare(const md::MarketDataConfig&) noexcept override {
    return {};
  }
  base::Status Start(std::span<const model::InstrumentId> ids,
                     std::uint64_t) noexcept override {
    if (ids.size() != 1) return {ErrorCode::kInvalidArgument, {}};
    id_ = ids[0];
    active_ = true;
    return {};
  }
  MarketDataPollResult Poll(std::uint64_t now,
                            model::QuoteTick& out) noexcept override {
    if (idle.Poll(now)) return {};
    if (poll_driven_idle && poll_calls != 0) {
      if (++idle_polls > 512) {
        return {MarketDataPollState::kError, {ErrorCode::kInternalError, {}}};
      }
      return {};
    }
    if (!continuous && poll_calls != 0) return {};
    if (poll_calls >= 512) {
      return {MarketDataPollState::kError, {ErrorCode::kInternalError, {}}};
    }
    ++poll_calls;
    out = {};
    out.source_id = {1};
    out.instrument_id = id_;
    out.sequence = static_cast<std::uint64_t>(poll_calls);
    out.receive_time_ns = now;
    out.bid_price = 1000;
    out.bid_qty = 100;
    out.ask_price = 1001;
    out.ask_qty = 200;
    out.flags = 1 | 2 | 4;
    return {MarketDataPollState::kEvent, {}};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    if (!active_) return {};
    if (idle.deadline) return idle.deadline;
    if (poll_driven_idle) return 0;
    return poll_calls == 0 || continuous ? 0 : 60000000000ULL;
  }
  bool is_poll_driven() const noexcept override { return poll_driven_idle; }
  base::Status Service(std::uint64_t now) noexcept override {
    if (!service_deadline || now < *service_deadline) return {};
    ++due_services;
    serviced_at = now;
    service_deadline.reset();
    return {fail_service ? ErrorCode::kIoError : ErrorCode::kOk, {}};
  }
  std::optional<std::uint64_t> next_service_deadline() const noexcept override {
    return active_ ? service_deadline : std::nullopt;
  }
  base::Status Stop() noexcept override {
    ++stop_calls;
    active_ = false;
    idle.deadline.reset();
    service_deadline.reset();
    return {};
  }
  const char* error_detail() const noexcept override {
    return fail_service ? "injected market maintenance failure" : "";
  }

 private:
  model::InstrumentId id_;
  bool active_ = false;
};

// Counts adapter-boundary facts while using the actual mock's scheduling and
// fill rules. Faults alter only normalized input or a send result.
class ObservedTradingAdapter final : public trading::TradingAdapter {
 public:
  bool fail_prepare = false;
  bool fail_start = false;
  bool duplicate_first = false;
  bool sequence_gap = false;
  bool fail_send = false;
  bool never_report = false;
  bool fail_account_read = false;
  bool invalid_account = false;
  std::int64_t price_improvement = 0;
  IdleClockStep idle;
  int send_orders = 0;
  int send_cancels = 0;
  int accepted = 0;
  int published = 0;
  int stop_calls = 0;
  int account_reads = 0;
  std::int64_t total_filled = 0;
  const TradingQuoteSource* observed_source = nullptr;
  std::array<int, 16> source_polls_at_reports = {};

  base::Status Prepare(const trading::TradingConfig& config) noexcept override {
    if (fail_prepare) return {ErrorCode::kIoError, {}};
    initial_account_ = config.initial_account;
    return mock_.Prepare(config);
  }
  base::Status Start(std::uint64_t now) noexcept override {
    if (fail_start) return {ErrorCode::kIoError, {}};
    return mock_.Start(now);
  }
  base::Status ReadAccountSnapshot(
      trading::TradingAccountSnapshot& out) noexcept override {
    ++account_reads;
    if (fail_account_read) return {ErrorCode::kIoError, {}};
    out = initial_account_;
    if (invalid_account) out.cash_balance = -1;
    return {};
  }
  base::Status SendOrder(model::ClientOrderId id,
                         const model::OrderRequest& request,
                         std::uint64_t now) noexcept override {
    ++send_orders;
    if (fail_send) return {ErrorCode::kIoError, {}};
    if (never_report) {
      waiting_ = true;
      return {};
    }
    return mock_.SendOrder(id, request, now);
  }
  base::Status SendCancel(model::ClientOrderId id,
                          const model::BrokerOrderId& broker,
                          model::CancelRequestId cancel_id,
                          std::uint64_t now) noexcept override {
    ++send_cancels;
    return mock_.SendCancel(id, broker, cancel_id, now);
  }
  trading::TradingPollResult Poll(
      std::uint64_t now, trading::TradingEvent& out) noexcept override {
    if (idle.Poll(now)) return {};
    if (pending_duplicate_) {
      pending_duplicate_ = false;
      out = duplicate_;
      ++published;
      return {trading::TradingPollState::kEvent, {}};
    }
    if (never_report) return {};
    auto result = mock_.Poll(now, out);
    if (result.state == trading::TradingPollState::kEvent) {
      if (observed_source != nullptr &&
          published < static_cast<int>(source_polls_at_reports.size())) {
        source_polls_at_reports[static_cast<std::size_t>(published)] =
            observed_source->poll_calls;
      }
      ++published;
      if (out.kind == model::OrderUpdateKind::kAccepted) ++accepted;
      if (out.kind == model::OrderUpdateKind::kFill) {
        out.fill.price += out.fill.side == model::OrderSide::kBuy
                              ? -price_improvement
                              : price_improvement;
        total_filled += out.fill.quantity;
      }
      if (published == 1) {
        if (sequence_gap) ++out.report_sequence;
        if (duplicate_first) {
          duplicate_ = out;
          pending_duplicate_ = true;
        }
      }
    }
    return result;
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    if (idle.deadline) return idle.deadline;
    if (pending_duplicate_) return 0;
    if (never_report) return {};
    return mock_.next_deadline();
  }
  bool is_drained() const noexcept override {
    return !waiting_ && !pending_duplicate_ && mock_.is_drained();
  }
  base::Status Stop() noexcept override {
    ++stop_calls;
    idle.deadline.reset();
    return mock_.Stop();
  }
  const char* error_detail() const noexcept override {
    if (fail_account_read) return "injected account read failure";
    return fail_send ? "injected uncertain send" : mock_.error_detail();
  }

 private:
  trading::MockTradingAdapter mock_;
  trading::TradingAccountSnapshot initial_account_;
  trading::TradingEvent duplicate_;
  bool pending_duplicate_ = false;
  bool waiting_ = false;
};

// Advances the shared monotonic clock only when Run asks to wait. A bounded
// watchdog converts accidental spinning or an unplanned wait into test failure.
class TradingLoopEnvironment final : public base::EventLoopEnvironment {
 public:
  explicit TradingLoopEnvironment(TradingClock& clock) : clock_(clock) {}
  const TradingQuoteSource* source = nullptr;
  const ObservedTradingAdapter* adapter = nullptr;
  bool stop_after_quote = false;
  bool stop_after_accepted = false;
  bool stop_after_send = false;
  std::uint64_t check_advance_ns = 0;
  int check_calls = 0;
  int wait_calls = 0;
  std::array<std::uint64_t, 128> waits = {};

  base::Status CheckStop(bool& out) noexcept override {
    if (++check_calls > 10000) return {ErrorCode::kInternalError, {}};
    clock_.now += check_advance_ns;
    out = (stop_after_quote && source != nullptr && source->poll_calls != 0) ||
          (adapter != nullptr &&
           ((stop_after_accepted && adapter->accepted != 0) ||
            (stop_after_send && adapter->send_orders != 0) ||
            (adapter->send_orders != 0 && adapter->is_drained())));
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept override {
    if (!timeout || wait_calls == static_cast<int>(waits.size())) {
      return {ErrorCode::kInternalError, {}};
    }
    waits[static_cast<std::size_t>(wait_calls++)] = *timeout;
    clock_.now += *timeout;
    return {};
  }

 private:
  TradingClock& clock_;
};

std::string TradingConfigJson(std::string_view mode,
                              std::uint32_t timeout_ms = 100,
                              std::int64_t initial_cash = 100000000000000,
                              std::int64_t available_qty = 800) {
  return std::string(R"({"adapter":"mock","mode":")") + std::string(mode) +
         R"(","report_interval_ms":1,"shutdown_timeout_ms":)" +
         std::to_string(timeout_ms) + R"(,"initial_cash":)" +
         std::to_string(initial_cash) +
         R"(,"initial_positions":[{"venue":"SSE","symbol":"600000",)"
         R"("total_qty":1000,"available_qty":)" +
         std::to_string(available_qty) + "}]}";
}

TEST(TradingRunnerTest, CleansUpPartialRuntimeInitializationBeforePluginLoad) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  for (const bool fail_prepare : {true, false}) {
    TradingClock clock;
    TradingQuoteSource source;
    ObservedTradingAdapter adapter;
    adapter.fail_prepare = fail_prepare;
    adapter.fail_start = !fail_prepare;
    TradingLoopEnvironment environment(clock);
    RunnerTestHarness runner(clock, environment, source, adapter);
    EXPECT_EQ(runner
                  .Handle({YQUANT_TEST_TRADING_NORMAL_PATH,
                           YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                           YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                           YQUANT_TEST_TRADING_CONFIG_PATH})
                  .code,
              ErrorCode::kIoError);
    EXPECT_STREQ(runner.error_stage(),
                 fail_prepare ? "trading_prepare" : "trading_start");
    EXPECT_EQ(adapter.account_reads, 0);
    EXPECT_EQ(adapter.stop_calls, 1);
    EXPECT_EQ(source.stop_calls, 1);
    EXPECT_TRUE(events.Read().empty());
  }
}

TEST(TradingRunnerTest,
     Fills200AsTwo100ShareExecutionsAndQueriesBeforeCallbacks) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.source = &source;
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(adapter.account_reads, 1);
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
  EXPECT_EQ(adapter.send_orders, 1);
  EXPECT_EQ(adapter.send_cancels, 0);
  EXPECT_EQ(adapter.total_filled, 200);
  EXPECT_EQ(source.poll_calls, 1);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(adapter.stop_calls, 1);
  EXPECT_EQ(clock.now, 3000000U);
  ASSERT_EQ(loop.wait_calls, 3);
  for (int index = 0; index < loop.wait_calls; ++index) {
    EXPECT_EQ(loop.waits[static_cast<std::size_t>(index)], 1000000U);
  }
}

TEST(TradingRunnerTest, QuantityOneHasOneNonzeroFill) {
  testing::ConfigFile config(
      R"({"strategy_id":"one","parameters":{"quantity":1}})");
  ASSERT_TRUE(config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                          config.path(),
                                          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                          YQUANT_TEST_TRADING_CONFIG_PATH})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPSQNAVFIfTD");
  EXPECT_EQ(adapter.total_filled, 1);
  EXPECT_EQ(adapter.published, 2);
}

struct TradingModeCase {
  const char* name;
  const char* mode;
  const char* plugin;
  bool stop_after_accepted;
  const char* trace;
};
class TradingModeTest : public ::testing::TestWithParam<TradingModeCase> {};
TEST_P(TradingModeTest, HandlesBusinessOutcomesAndDrainsBeforeStop) {
  testing::ConfigFile config(TradingConfigJson(GetParam().mode));
  ASSERT_TRUE(config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  loop.stop_after_accepted = GetParam().stop_after_accepted;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{
              GetParam().plugin, YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
              YQUANT_TEST_MARKET_DATA_CONFIG_PATH, config.path()})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), GetParam().trace);
}
INSTANTIATE_TEST_SUITE_P(
    MockOutcomes, TradingModeTest,
    ::testing::Values(
        TradingModeCase{"Rejected", "reject", YQUANT_TEST_TRADING_NORMAL_PATH,
                        false, "CPSQNRrTD"},
        TradingModeCase{"HostCancelsWorking", "working",
                        YQUANT_TEST_TRADING_NORMAL_PATH, true, "CPSQNAXcTD"},
        TradingModeCase{"StrategyCancelsWorking", "working",
                        YQUANT_TEST_TRADING_CANCEL_PATH, false, "CPSQNAKXcTD"},
        TradingModeCase{"PartialFillBeforeCancel", "fill",
                        YQUANT_TEST_TRADING_CANCEL_PATH, false,
                        "CPSQNAKVFHXcTD"},
        TradingModeCase{"FilledBeforeQueuedCancelResult", "fill",
                        YQUANT_TEST_TRADING_PARTIAL_CANCEL_PATH, false,
                        "CPSQNAVKFHVFHJfTD"}),
    [](const ::testing::TestParamInfo<TradingModeCase>& info) {
      return info.param.name;
    });

TEST(TradingRunnerTest, StopCancelsAll200UnsentOrdersWithoutSendingAny) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.source = &source;
  loop.adapter = &adapter;
  loop.stop_after_quote = true;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_BULK_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(adapter.send_orders, 0);
  const auto trace = events.Read();
  EXPECT_EQ(trace, "CPSQ" + std::string(200, 'N') + std::string(200, 'B') +
                       std::string(200, 'c') + "TD");
}

struct TradingCallbackFailure {
  const char* name;
  const char* plugin;
  const char* prefix;
  const char* stage;
};
class TradingCallbackFailureTest
    : public ::testing::TestWithParam<TradingCallbackFailure> {};
TEST_P(TradingCallbackFailureTest, SuppressesCallbacksButCompletesHostDrain) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{GetParam().plugin,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  EXPECT_EQ(runner.Run().code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), GetParam().stage);
  const auto trace = events.Read();
  const auto prefix_size = std::char_traits<char>::length(GetParam().prefix);
  EXPECT_EQ(trace.substr(0, prefix_size), GetParam().prefix);
  ASSERT_EQ(trace.size(), prefix_size + 3);
  EXPECT_TRUE(trace[prefix_size] == 'c' || trace[prefix_size] == 'f');
  EXPECT_EQ(trace.substr(prefix_size + 1), "TD");
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'T'), 1);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'D'), 1);
  EXPECT_TRUE(adapter.is_drained());
}
INSTANTIATE_TEST_SUITE_P(
    CallbackErrors, TradingCallbackFailureTest,
    ::testing::Values(
        TradingCallbackFailure{"OrderUpdate",
                               YQUANT_TEST_TRADING_UPDATE_FAIL_PATH, "CPSQNA",
                               "order_update"},
        TradingCallbackFailure{"Fill", YQUANT_TEST_TRADING_FILL_FAIL_PATH,
                               "CPSQNAVFH", "fill"}),
    [](const ::testing::TestParamInfo<TradingCallbackFailure>& info) {
      return info.param.name;
    });

TEST(TradingRunnerTest, DuplicateReportDoesNotRepeatBusinessCallbacks) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.duplicate_first = true;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(adapter.published, 4);
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
}

TEST(TradingRunnerTest, SequenceGapStopsWithoutApplyingTheInvalidReport) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.sequence_gap = true;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  EXPECT_FALSE(runner.Run().ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), "trading_event");
  EXPECT_EQ(events.Read(), "CPSQNpTD");
  EXPECT_NE(std::string(runner.error_detail()).find("client_order_id=1"),
            std::string::npos);
  EXPECT_NE(std::string(runner.error_detail()).find("sequence=2"),
            std::string::npos);
}

TEST(TradingRunnerTest, UncertainSendIsNotRetriedOrLocallyCanceled) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.fail_send = true;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "trading_send");
  EXPECT_EQ(adapter.send_orders, 1);
  EXPECT_EQ(events.Read(), "CPSQNpTD");
}

TEST(TradingRunnerTest, PendingAcceptanceTimesOutAtTheOriginalDrainDeadline) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.never_report = true;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  loop.stop_after_send = true;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  EXPECT_FALSE(runner.Run().ok());
  EXPECT_STREQ(runner.error_stage(), "trading_drain_timeout");
  EXPECT_EQ(clock.now, 100000000U);
  EXPECT_EQ(adapter.send_orders, 1);
  EXPECT_EQ(events.Read(), "CPSQNpTD");
  EXPECT_NE(std::string(runner.error_detail()).find("deadline"),
            std::string::npos);
  EXPECT_LT(loop.check_calls, 100);
}

TEST(TradingRunnerTest, ReadyQuotesDoNotStarveTradingAndShareTheBatchBudget) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  source.continuous = true;
  ObservedTradingAdapter adapter;
  adapter.observed_source = &source;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  loop.check_advance_ns = 1000000;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  ASSERT_EQ(adapter.published, 3);
  for (std::size_t index = 1; index < 3; ++index) {
    EXPECT_LE(adapter.source_polls_at_reports[index] -
                  adapter.source_polls_at_reports[index - 1],
              64);
  }
  EXPECT_EQ(loop.wait_calls, 0);
  const auto trace = events.Read();
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'A'), 1);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'V'), 2);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'F'), 2);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'N'), 1);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'T'), 1);
  EXPECT_EQ(source.poll_calls, std::count(trace.begin(), trace.end(), 'Q'));
}

class MarketIdleDeadlineTest : public ::testing::TestWithParam<bool> {};

TEST_P(MarketIdleDeadlineTest, ValidatesIdleAgainstTheTimePassedToPoll) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  source.idle.clock = &clock;
  source.idle.retain_due_deadline = GetParam();
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  const auto result = runner.Run();
  EXPECT_EQ(source.idle.idle_returns, 1);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  if (GetParam()) {
    EXPECT_EQ(result.code, ErrorCode::kCorrupt);
    EXPECT_STREQ(runner.error_stage(), "market_data_poll");
    EXPECT_EQ(adapter.send_orders, 0);
    EXPECT_EQ(events.Read(), "CPSTD");
  } else {
    ASSERT_TRUE(result.ok()) << runner.error_detail();
    EXPECT_EQ(source.poll_calls, 1);
    EXPECT_EQ(adapter.total_filled, 200);
    EXPECT_EQ(clock.now, 3000001U);
    EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
  }
}

INSTANTIATE_TEST_SUITE_P(MarketIdleClockRace, MarketIdleDeadlineTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "StillDueAtPollInput"
                                             : "BecomesDueDuringPoll";
                         });

class TradingIdleDeadlineTest : public ::testing::TestWithParam<bool> {};

TEST_P(TradingIdleDeadlineTest, ValidatesIdleAgainstTheTimePassedToPoll) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.idle.clock = &clock;
  adapter.idle.retain_due_deadline = GetParam();
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  const auto result = runner.Run();
  EXPECT_EQ(adapter.idle.idle_returns, 1);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  if (GetParam()) {
    EXPECT_EQ(result.code, ErrorCode::kCorrupt);
    EXPECT_STREQ(runner.error_stage(), "trading_poll");
    EXPECT_EQ(adapter.accepted, 0);
    EXPECT_EQ(events.Read(), "CPSQNpTD");
  } else {
    ASSERT_TRUE(result.ok()) << runner.error_detail();
    EXPECT_EQ(adapter.total_filled, 200);
    EXPECT_EQ(clock.now, 3000001U);
    EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
  }
}

INSTANTIATE_TEST_SUITE_P(TradingIdleClockRace, TradingIdleDeadlineTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "StillDueAtPollInput"
                                             : "BecomesDueDuringPoll";
                         });

struct SellAccountCase {
  const char* name;
  const char* mode;
  const char* plugin;
  const char* trace;
};
class SellAccountTest : public ::testing::TestWithParam<SellAccountCase> {};

TEST_P(SellAccountTest, UpdatesCashAndSellReservationsBeforeCallbacks) {
  testing::ConfigFile strategy_config(
      R"({"strategy_id":"sell-account","parameters":{"side":"sell"}})");
  testing::ConfigFile trading_config(TradingConfigJson(GetParam().mode));
  ASSERT_TRUE(strategy_config.valid());
  ASSERT_TRUE(trading_config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{GetParam().plugin, strategy_config.path(),
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  trading_config.path()})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), GetParam().trace);
  EXPECT_EQ(adapter.send_orders, 1);
}

INSTANTIATE_TEST_SUITE_P(
    SellAccounts, SellAccountTest,
    ::testing::Values(
        SellAccountCase{"Filled", "fill", YQUANT_TEST_TRADING_NORMAL_PATH,
                        "CPSQNAVFHVFHfTD"},
        SellAccountCase{"PartialThenCanceled", "fill",
                        YQUANT_TEST_TRADING_CANCEL_PATH, "CPSQNAKVFHXcTD"},
        SellAccountCase{"Rejected", "reject", YQUANT_TEST_TRADING_NORMAL_PATH,
                        "CPSQNRrTD"}),
    [](const ::testing::TestParamInfo<SellAccountCase>& info) {
      return info.param.name;
    });

TEST(TradingRunnerTest, ImprovedExecutionPriceReleasesUnusedBuyReservation) {
  testing::ConfigFile strategy_config(
      R"({"strategy_id":"improved-price","parameters":{"price_improvement":1}})");
  ASSERT_TRUE(strategy_config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.price_improvement = 1;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                          strategy_config.path(),
                                          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                          YQUANT_TEST_TRADING_CONFIG_PATH})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
}

class InsufficientAccountTest : public ::testing::TestWithParam<bool> {};

TEST_P(InsufficientAccountTest,
       LocalFailurePreservesOutputBookCashAndPosition) {
  const bool sell = GetParam();
  testing::ConfigFile strategy_config(
      sell ? R"({"strategy_id":"no-shares","parameters":{"side":"sell"}})"
           : R"({"strategy_id":"no-cash","parameters":{}})");
  testing::ConfigFile trading_config(TradingConfigJson(
      "fill", 100, sell ? 100000000000000 : 0, sell ? 1 : 800));
  ASSERT_TRUE(strategy_config.valid());
  ASSERT_TRUE(trading_config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  loop.source = &source;
  loop.stop_after_quote = true;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_TRADING_INSUFFICIENT_PATH,
                                          strategy_config.path(),
                                          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                          trading_config.path()})
                  .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPSQLTD");
  EXPECT_EQ(adapter.send_orders, 0);
  EXPECT_EQ(adapter.send_cancels, 0);
}

INSTANTIATE_TEST_SUITE_P(LocalAccountRejections, InsufficientAccountTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "InsufficientShares"
                                             : "InsufficientCash";
                         });

class AccountBootstrapFailureTest : public ::testing::TestWithParam<bool> {};

TEST_P(AccountBootstrapFailureTest, RejectsBeforeFactoryAndOnStart) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  ObservedTradingAdapter adapter;
  adapter.fail_account_read = GetParam();
  adapter.invalid_account = !GetParam();
  TradingLoopEnvironment loop(clock);
  RunnerTestHarness runner(clock, loop, source, adapter);
  EXPECT_FALSE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok());
  EXPECT_STREQ(runner.error_stage(), "trading_account");
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(adapter.account_reads, 1);
  EXPECT_EQ(adapter.stop_calls, 1);
  EXPECT_EQ(adapter.send_orders, 0);
  EXPECT_EQ(source.poll_calls, 0);
  EXPECT_TRUE(events.Read().empty());
}

INSTANTIATE_TEST_SUITE_P(AccountBootstrapFailures, AccountBootstrapFailureTest,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& info) {
                           return info.param ? "ReadFailure"
                                             : "InvalidSnapshot";
                         });

TEST(TradingRunnerTest, PollDrivenEmptyReadsDoNotStarveReportsOrStopping) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  source.poll_driven_idle = true;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  loop.check_advance_ns = 1000000;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(source.poll_calls, 1);
  EXPECT_GT(source.idle_polls, 0);
  EXPECT_EQ(adapter.published, 3);
  EXPECT_EQ(adapter.total_filled, 200);
  EXPECT_EQ(loop.wait_calls, 0);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(adapter.stop_calls, 1);
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
}

TEST(TradingRunnerTest, ColdServiceDeadlinePrecedesTheNextTradingReport) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  source.service_deadline = 500000;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_TRADING_NORMAL_PATH,
                                  YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                                  YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
                                  YQUANT_TEST_TRADING_CONFIG_PATH})
          .ok())
      << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  ASSERT_GE(loop.wait_calls, 2);
  EXPECT_EQ(loop.waits[0], 500000U);
  EXPECT_EQ(loop.waits[1], 500000U);
  EXPECT_EQ(source.due_services, 1);
  EXPECT_EQ(source.serviced_at, 500000U);
  EXPECT_EQ(adapter.total_filled, 200);
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
}

TEST(TradingRunnerTest, MarketServiceErrorStillDrainsTheWorkingOrder) {
  testing::ConfigFile config(TradingConfigJson("working"));
  ASSERT_TRUE(config.valid());
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  source.service_deadline = 2000000;
  source.fail_service = true;
  ObservedTradingAdapter adapter;
  TradingLoopEnvironment loop(clock);
  loop.adapter = &adapter;
  RunnerTestHarness runner(clock, loop, source, adapter);
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{
                      YQUANT_TEST_TRADING_NORMAL_PATH,
                      YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
                      YQUANT_TEST_MARKET_DATA_CONFIG_PATH, config.path()})
                  .ok())
      << runner.error_detail();
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "market_data_service");
  EXPECT_NE(std::string(runner.error_detail())
                .find("injected market maintenance failure"),
            std::string::npos);
  EXPECT_EQ(source.serviced_at, 2000000U);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(adapter.send_cancels, 1);
  EXPECT_TRUE(adapter.is_drained());
  const auto trace = events.Read();
  ASSERT_GE(trace.size(), 9U);
  EXPECT_EQ(trace.substr(0, 6), "CPSQNA");
  EXPECT_EQ(trace.substr(trace.size() - 3), "cTD");
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'T'), 1);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'D'), 1);
}

}  // namespace
}  // namespace yquant::strategyd
