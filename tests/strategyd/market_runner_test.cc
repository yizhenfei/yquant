#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <span>
#include <string>

#include "gtest/gtest.h"
#include "strategyd/event_loop_environment.h"
#include "strategyd/market_data_source.h"
#include "test_strategy_runner.h"
#include "yquant/model/identifiers.h"
#include "yquant/model/quote_tick.h"

namespace yquant::strategyd {
namespace {

using base::ErrorCode;

// The fixture records callbacks through a pipe without changing the SDK.
class QuoteEventPipe {
 public:
  QuoteEventPipe() {
    const char* previous = std::getenv("YQUANT_TEST_EVENT_FD");
    had_previous_ = previous != nullptr;
    if (had_previous_) {
      previous_ = previous;
    }
    if (pipe(fds_) != 0 || fcntl(fds_[0], F_SETFL, O_NONBLOCK) == -1) {
      return;
    }
    const std::string fd = std::to_string(fds_[1]);
    valid_ = setenv("YQUANT_TEST_EVENT_FD", fd.c_str(), 1) == 0;
  }

  ~QuoteEventPipe() {
    if (had_previous_) {
      setenv("YQUANT_TEST_EVENT_FD", previous_.c_str(), 1);
    } else {
      unsetenv("YQUANT_TEST_EVENT_FD");
    }
    for (const int fd : fds_) {
      if (fd >= 0) {
        close(fd);
      }
    }
  }

  bool valid() const { return valid_; }

  std::string Read() {
    std::string events;
    char buffer[64];
    for (;;) {
      const ssize_t count = read(fds_[0], buffer, sizeof(buffer));
      if (count > 0) {
        events.append(buffer, static_cast<std::size_t>(count));
      } else if (count == -1 && errno == EINTR) {
        continue;
      } else {
        return events;
      }
    }
  }

 private:
  int fds_[2] = {-1, -1};
  bool valid_ = false;
  bool had_previous_ = false;
  std::string previous_;
};

void ExpectQuoteTrace(std::string events, const char* expected) {
  events.resize(events.find('U') == std::string::npos ? events.size()
                                                      : events.find('U'));
  EXPECT_EQ(events, expected);
}

model::QuoteTick Quote(std::uint64_t sequence = 1) {
  model::QuoteTick tick;
  tick.source_id = {1};
  EXPECT_EQ(model::MakeInstrumentId("SSE", "600000", tick.instrument_id),
            ErrorCode::kOk);
  tick.sequence = sequence;
  tick.event_time_ns = 123456789;
  tick.receive_time_ns = 0;
  tick.bid_price = 1000;
  tick.bid_qty = 100;
  tick.ask_price = 1001;
  tick.ask_qty = 200;
  tick.flags = model::kKnownQuoteFlags;
  return tick;
}

// Injects bounded synchronous source behavior; no worker or real-time wait is
// involved. The strategy fixture independently validates callback ordering.
class ControlledSource final : public MarketDataSource {
 public:
  bool fail_prepare = false;
  bool fail_start = false;
  bool fail_poll = false;
  bool fail_stop = false;
  bool repeat_sequence = false;
  bool poll_driven_idle = false;
  int prepare_calls = 0;
  int start_calls = 0;
  int poll_calls = 0;
  int stop_calls = 0;

  // Borrows the finite script through the runner's lifetime. Exhaustion removes
  // its deadline so Run returns to the environment before requesting more data.
  void SetQuotes(std::span<const model::QuoteTick> quotes) noexcept {
    quotes_ = quotes;
    scripted_ = true;
  }

  bool exhausted() const noexcept {
    return scripted_ && next_quote_ == quotes_.size();
  }

  base::Status Prepare(const MarketDataConfig&) noexcept override {
    ++prepare_calls;
    return fail_prepare ? Fail("injected prepare failure") : base::Status{};
  }

  base::Status Start(std::span<const model::InstrumentId> subscriptions,
                     std::uint64_t) noexcept override {
    ++start_calls;
    if (subscriptions.size() != 1 || fail_start) {
      return Fail("injected start failure");
    }
    next_quote_ = 0;
    active_ = true;
    return {};
  }

  MarketDataPollResult Poll(std::uint64_t,
                            model::QuoteTick& out) noexcept override {
    ++poll_calls;
    if (fail_poll) {
      return {MarketDataPollState::kError, Fail("injected poll failure")};
    }
    if (poll_driven_idle) return {};
    if (scripted_) {
      if (exhausted()) {
        return {MarketDataPollState::kIdle, {}};
      }
      out = quotes_[next_quote_++];
      return {MarketDataPollState::kEvent, {}};
    }
    model::QuoteTick tick;
    tick.source_id = {1};
    tick.instrument_id.value = (std::uint64_t{1} << 32) | 600000;
    tick.sequence =
        repeat_sequence ? 1 : static_cast<std::uint64_t>(poll_calls);
    tick.bid_price = 1000;
    tick.bid_qty = 100;
    tick.ask_price = 1001;
    tick.ask_qty = 200;
    tick.flags = 1 | 2 | 4;
    out = tick;
    return {MarketDataPollState::kEvent, {}};
  }

  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return active_ && !exhausted() ? std::optional<std::uint64_t>{0}
                                   : std::nullopt;
  }

  bool is_poll_driven() const noexcept override { return poll_driven_idle; }

  base::Status Stop() noexcept override {
    ++stop_calls;
    active_ = false;
    return fail_stop ? Fail("injected stop failure") : base::Status{};
  }

  const char* error_detail() const noexcept override { return error_; }

 private:
  base::Status Fail(const char* error) noexcept {
    if (error_[0] == '\0') {
      error_ = error;
    }
    return {ErrorCode::kIoError, {}};
  }

  bool active_ = false;
  bool scripted_ = false;
  std::span<const model::QuoteTick> quotes_;
  std::size_t next_quote_ = 0;
  const char* error_ = "";
};

// Test environment never sleeps: it stops at a selected source boundary or
// exposes a deterministic wait/check failure to the runner's cleanup path.
class TestEventLoopEnvironment final : public EventLoopEnvironment {
 public:
  const ControlledSource* source = nullptr;
  bool fail_check = false;
  bool fail_wait = false;
  bool stop_after_start = false;
  bool stop_when_exhausted = false;
  int stop_after_polls = 0;
  int check_calls = 0;
  int wait_calls = 0;

  base::Status CheckStop(bool& out) noexcept override {
    ++check_calls;
    if (fail_check) {
      return {ErrorCode::kIoError, {}};
    }
    // A broken stop condition must fail deterministically instead of running
    // the always-ready test source forever.
    if (source != nullptr && source->poll_calls > 256) {
      return {ErrorCode::kInternalError, {}};
    }
    out = source != nullptr &&
          ((stop_after_start && source->start_calls != 0) ||
           (stop_when_exhausted && source->exhausted()) ||
           (stop_after_polls != 0 && source->poll_calls >= stop_after_polls));
    return {};
  }

  base::Status Wait(std::optional<std::uint64_t>) noexcept {
    ++wait_calls;
    return {fail_wait ? ErrorCode::kIoError : ErrorCode::kInternalError, {}};
  }
};

TEST(MarketRunnerTest, UpdatesSnapshotBeforeCallbackAndDropsOldSequences) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  std::array quotes = {Quote(2), Quote(2), Quote(1), Quote(9)};
  quotes[1].bid_price = 500;
  quotes[2].bid_price = 500;
  ControlledSource source;
  source.SetQuotes(quotes);
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_when_exhausted = true;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok())
      << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPS");
  EXPECT_TRUE(runner.next_deadline().has_value());
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(source.poll_calls, 4);
  EXPECT_EQ(loop.wait_calls, 0);
  EXPECT_EQ(runner.ignored_quote_count(), 2U);
  ExpectQuoteTrace(events.Read(), "QQTD");
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_TRUE(runner.Handle(Stop{}).ok());
  EXPECT_EQ(runner.ignored_quote_count(), 2U);
  EXPECT_FALSE(runner.next_deadline().has_value());
  EXPECT_EQ(runner.Run(loop).code, ErrorCode::kInvalidState);
  EXPECT_TRUE(events.Read().empty());
}

TEST(MarketRunnerTest, DeliversMissingSidesLockedCrossedAndNontradableQuotes) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  std::array quotes = {Quote(1), Quote(2), Quote(3), Quote(4), Quote(5)};
  quotes[1].bid_price = 0;
  quotes[1].bid_qty = 0;
  quotes[1].flags = 2 | 4 | 8;
  quotes[2].bid_price = quotes[2].ask_price;
  quotes[3].bid_price = quotes[3].ask_price + 1;
  quotes[3].flags = 1 | 2;
  quotes[3].event_time_ns = 0;
  quotes[4].bid_price = 0;
  quotes[4].bid_qty = 0;
  quotes[4].ask_price = 0;
  quotes[4].ask_qty = 0;
  quotes[4].flags = 0;
  quotes[4].event_time_ns = 0;
  ControlledSource source;
  source.SetQuotes(quotes);
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_when_exhausted = true;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(source.poll_calls, 5);
  ExpectQuoteTrace(events.Read(), "CPSQQQQQTD");
}

struct InvalidQuote {
  const char* name;
  void (*modify)(model::QuoteTick&);
};

class InvalidQuoteTest : public ::testing::TestWithParam<InvalidQuote> {};

TEST_P(InvalidQuoteTest, RejectsBeforeDispatchAndStopsTheStartedStrategy) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  std::array quotes = {Quote(2), Quote(1), Quote(3)};
  GetParam().modify(quotes[1]);
  ControlledSource source;
  source.SetQuotes(quotes);
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_when_exhausted = true;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_FALSE(runner.Run(loop).ok());
  EXPECT_EQ(source.poll_calls, 2);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), "quote");
  ExpectQuoteTrace(events.Read(), "CPSQTD");
  EXPECT_TRUE(runner.Handle(Stop{}).ok());
  EXPECT_TRUE(events.Read().empty());
}

INSTANTIATE_TEST_SUITE_P(
    ProtocolErrors, InvalidQuoteTest,
    ::testing::Values(
        InvalidQuote{"WrongSource", [](auto& tick) { tick.source_id = {2}; }},
        InvalidQuote{"ZeroSource", [](auto& tick) { tick.source_id = {}; }},
        InvalidQuote{"UnknownInstrument",
                     [](auto& tick) { tick.instrument_id.value += 1; }},
        InvalidQuote{"ZeroInstrument",
                     [](auto& tick) { tick.instrument_id = {}; }},
        InvalidQuote{"ZeroSequence", [](auto& tick) { tick.sequence = 0; }},
        InvalidQuote{"UnknownFlags", [](auto& tick) { tick.flags |= 0x80; }},
        InvalidQuote{"NegativeBidQuantity",
                     [](auto& tick) { tick.bid_qty = -1; }},
        InvalidQuote{"NegativeAskQuantity",
                     [](auto& tick) { tick.ask_qty = -1; }},
        InvalidQuote{"PresentSideZeroQuantity",
                     [](auto& tick) { tick.bid_qty = 0; }},
        InvalidQuote{"AbsentSideHasPrice",
                     [](auto& tick) { tick.flags &= ~1; }},
        InvalidQuote{"AbsentEventTimeNonzero",
                     [](auto& tick) { tick.flags &= ~8; }}),
    [](const ::testing::TestParamInfo<InvalidQuote>& info) {
      return info.param.name;
    });

TEST(MarketRunnerTest, TickFailurePreservesDiagnosticAndPairsStop) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  const std::array quotes = {Quote(42), Quote(43)};
  ControlledSource source;
  source.SetQuotes(quotes);
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_when_exhausted = true;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_TICK_FAIL_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(runner.Run(loop).code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(source.poll_calls, 1);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), "tick");
  EXPECT_EQ(runner.strategy_id(), "test-quotes");
  EXPECT_NE(std::string(runner.error_detail()).find("42"), std::string::npos);
  ExpectQuoteTrace(events.Read(), "CPSQTD");
  EXPECT_EQ(runner.Run(loop).code, ErrorCode::kInvalidState);
  EXPECT_TRUE(events.Read().empty());
}

TEST(MarketRunnerTest, StartFailureNeverActivatesOrDispatchesTheSource) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_polls = 1;
  TestStrategyRunner runner(nullptr, &source);
  EXPECT_FALSE(
      runner
          .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTE_START_FAIL_PATH,
                               YQUANT_TEST_QUOTE_CONFIG_PATH,
                               YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
          .ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), "start");
  ExpectQuoteTrace(events.Read(), "CPSD");
  EXPECT_EQ(source.start_calls, 0);
  EXPECT_EQ(source.poll_calls, 0);
  EXPECT_FALSE(runner.next_deadline().has_value());
  EXPECT_EQ(runner.Run(loop).code, ErrorCode::kInvalidState);
  EXPECT_TRUE(events.Read().empty());
}

TEST(MarketRunnerTest, LimitsEachReadyBatchTo64Events) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  // Requests are observed at a batch boundary, even if the first poll makes
  // a stop request ready in this environment.
  loop.stop_after_polls = 1;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(events.Read(), "CPS");
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(source.poll_calls, 64);
  ExpectQuoteTrace(events.Read(), (std::string(64, 'Q') + "TD").c_str());
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(loop.wait_calls, 0);
}

TEST(MarketRunnerTest, RepeatedSequencesStillConsumeTheBatchBudget) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  source.repeat_sequence = true;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_polls = 128;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(events.Read(), "CPS");
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(source.poll_calls, 128);
  EXPECT_EQ(runner.ignored_quote_count(), 127U);
  EXPECT_GE(loop.check_calls, 3);
  EXPECT_EQ(loop.wait_calls, 0);
  ExpectQuoteTrace(events.Read(), "QTD");
}

struct SourceFailure {
  const char* name;
  bool ControlledSource::* failure;
  const char* stage;
  const char* detail;
};

class SourceFailureTest : public ::testing::TestWithParam<SourceFailure> {};

TEST_P(SourceFailureTest, RetainsErrorAndPairsLifecycleCallbacks) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  source.*(GetParam().failure) = true;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_start = source.fail_stop;
  TestStrategyRunner runner(nullptr, &source);
  const auto start = runner.Handle(LoadAndStart{
      YQUANT_TEST_PLUGIN_QUOTES_PATH, YQUANT_TEST_QUOTE_CONFIG_PATH,
      YQUANT_TEST_MARKET_DATA_CONFIG_PATH});
  if (source.fail_prepare || source.fail_start) {
    EXPECT_FALSE(start.ok());
  } else {
    ASSERT_TRUE(start.ok()) << runner.error_detail();
    EXPECT_FALSE(runner.Run(loop).ok());
  }
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), GetParam().stage);
  EXPECT_NE(std::string(runner.error_detail()).find(GetParam().detail),
            std::string::npos);
  if (source.fail_prepare) {
    EXPECT_EQ(source.start_calls, 0);
    EXPECT_TRUE(events.Read().empty());
  } else {
    EXPECT_EQ(source.stop_calls, 1);
    ExpectQuoteTrace(events.Read(), "CPSTD");
  }
  EXPECT_TRUE(runner.Handle(Stop{}).ok());
  EXPECT_TRUE(events.Read().empty());
}

INSTANTIATE_TEST_SUITE_P(
    SourceErrors, SourceFailureTest,
    ::testing::Values(
        SourceFailure{"Prepare", &ControlledSource::fail_prepare,
                      "market_data_prepare", "injected prepare failure"},
        SourceFailure{"Start", &ControlledSource::fail_start,
                      "market_data_start", "injected start failure"},
        SourceFailure{"Poll", &ControlledSource::fail_poll, "market_data_poll",
                      "injected poll failure"},
        SourceFailure{"Stop", &ControlledSource::fail_stop, "market_data_stop",
                      "injected stop failure"}),
    [](const ::testing::TestParamInfo<SourceFailure>& info) {
      return info.param.name;
    });

TEST(MarketRunnerTest, PollErrorSurvivesSourceStopFailure) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  source.fail_poll = true;
  source.fail_stop = true;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_FALSE(runner.Run(loop).ok());
  EXPECT_STREQ(runner.error_stage(), "market_data_poll");
  EXPECT_NE(std::string(runner.error_detail()).find("injected poll failure"),
            std::string::npos);
  ExpectQuoteTrace(events.Read(), "CPSTD");
}

TEST(MarketRunnerTest, ReadySourceChecksStopAfterOneBoundedBatch) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_polls = 64;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(source.poll_calls, 64);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_GE(loop.check_calls, 2);
  EXPECT_EQ(loop.wait_calls, 0);
  ExpectQuoteTrace(events.Read(),
                   ("CPS" + std::string(64, 'Q') + "TD").c_str());
}

TEST(MarketRunnerTest, BusyPollingIdleChecksStopWithoutWaitingOrCorruption) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  source.poll_driven_idle = true;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_polls = 3;
  TestStrategyRunner runner(nullptr, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(runner.Run(loop).ok()) << runner.error_detail();
  EXPECT_EQ(source.poll_calls, 3);
  EXPECT_EQ(loop.wait_calls, 0);
  EXPECT_GE(loop.check_calls, 4);
  EXPECT_EQ(source.stop_calls, 1);
  ExpectQuoteTrace(events.Read(), "CPSTD");
}

TEST(MarketRunnerTest, StopAfterSourceActivationDoesNotBeginPolling) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  ControlledSource source;
  TestEventLoopEnvironment loop;
  loop.source = &source;
  loop.stop_after_start = true;
  TestStrategyRunner runner(nullptr, &source, &loop);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(source.start_calls, 1);
  EXPECT_EQ(source.poll_calls, 0);
  EXPECT_EQ(source.stop_calls, 1);
  ExpectQuoteTrace(events.Read(), "CPSTD");
}

// Maintenance deadlines must wake a waiting host even when no quote is due.
class MaintenanceSource final : public MarketDataSource {
 public:
  std::optional<std::uint64_t> quote_deadline;
  int poll_calls = 0;
  int stop_calls = 0;

  base::Status Prepare(const MarketDataConfig&) noexcept override { return {}; }
  base::Status Start(std::span<const model::InstrumentId>,
                     std::uint64_t) noexcept override {
    return {};
  }
  MarketDataPollResult Poll(std::uint64_t,
                            model::QuoteTick&) noexcept override {
    ++poll_calls;
    return {MarketDataPollState::kError, {ErrorCode::kInternalError, {}}};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return quote_deadline;
  }
  base::Status Service(std::uint64_t now) noexcept override {
    return now < 100 ? base::Status{} : base::Status{ErrorCode::kIoError, {}};
  }
  std::optional<std::uint64_t> next_service_deadline() const noexcept override {
    return 100;
  }
  base::Status Stop() noexcept override {
    ++stop_calls;
    return {};
  }
  const char* error_detail() const noexcept override {
    return "injected maintenance failure";
  }
};

class MaintenanceClock final : public MonotonicClock {
 public:
  std::uint64_t now = 0;
  std::uint64_t Now() const noexcept override { return now; }
};

class MaintenanceEnvironment final : public EventLoopEnvironment {
 public:
  explicit MaintenanceEnvironment(MaintenanceClock& clock) : clock_(clock) {}
  base::Status CheckStop(bool& out) noexcept override {
    out = false;
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept {
    if (!timeout || *timeout != 100 || wait_calls != 0)
      return {ErrorCode::kInternalError, {}};
    ++wait_calls;
    clock_.now += *timeout;
    return {};
  }
  int wait_calls = 0;

 private:
  MaintenanceClock& clock_;
};

struct MaintenanceDeadline {
  const char* name;
  std::optional<std::uint64_t> quote_deadline;
};

class MaintenanceDeadlineTest
    : public ::testing::TestWithParam<MaintenanceDeadline> {};

TEST_P(MaintenanceDeadlineTest, ServicesBeforeQuoteDeadlineOrIndefiniteWait) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  MaintenanceSource source;
  source.quote_deadline = GetParam().quote_deadline;
  MaintenanceClock clock;
  MaintenanceEnvironment environment(clock);
  TestStrategyRunner runner(&clock, &source);
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_QUOTES_PATH,
                                       YQUANT_TEST_QUOTE_CONFIG_PATH,
                                       YQUANT_TEST_MARKET_DATA_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(runner.next_deadline(), 100U);
  EXPECT_EQ(runner.Run(environment).code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "market_data_service");
  EXPECT_STREQ(runner.error_detail(), "injected maintenance failure");
  EXPECT_EQ(source.poll_calls, 0);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(environment.wait_calls, 1);
  ExpectQuoteTrace(events.Read(), "CPSTD");
}

INSTANTIATE_TEST_SUITE_P(
    MaintenanceDeadlines, MaintenanceDeadlineTest,
    ::testing::Values(MaintenanceDeadline{"DistantQuote", 1000},
                      MaintenanceDeadline{"NoQuote", std::nullopt}),
    [](const ::testing::TestParamInfo<MaintenanceDeadline>& info) {
      return info.param.name;
    });

struct LoopFailure {
  const char* name;
  bool check;
};

class LoopFailureTest : public ::testing::TestWithParam<LoopFailure> {};

TEST_P(LoopFailureTest, StopsAndDestroysTheStartedStrategy) {
  QuoteEventPipe events;
  ASSERT_TRUE(events.valid());
  TestEventLoopEnvironment loop;
  loop.fail_check = GetParam().check;
  loop.fail_wait = !GetParam().check;
  TestStrategyRunner runner;
  ASSERT_TRUE(runner
                  .Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                       YQUANT_TEST_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(runner.Run(loop).code, ErrorCode::kIoError);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_NE(runner.error_detail()[0], '\0');
  EXPECT_EQ(loop.wait_calls, GetParam().check ? 0 : 1);
  ExpectQuoteTrace(events.Read(), "CPSTD");
}

INSTANTIATE_TEST_SUITE_P(LoopErrors, LoopFailureTest,
                         ::testing::Values(LoopFailure{"CheckStop", true},
                                           LoopFailure{"Wait", false}),
                         [](const ::testing::TestParamInfo<LoopFailure>& info) {
                           return info.param.name;
                         });

}  // namespace
}  // namespace yquant::strategyd
