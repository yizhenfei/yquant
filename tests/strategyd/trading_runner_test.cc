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

// Drives only the host clock and stop condition; the service supplies reports.
class TradingLoopEnvironment final : public TestLoopEnvironment {
 public:
  explicit TradingLoopEnvironment(TradingClock& clock) : clock_(clock) {}
  testing::MockTradingService* service = nullptr;
  const TradingQuoteSource* source = nullptr;
  int stop_after_events = 3;
  bool stop_after_quote = false;
  int checks = 0;
  std::vector<std::uint64_t> waits;
  base::Status CheckStop(bool& out) noexcept override {
    if (++checks > 10000) return {ErrorCode::kInternalError, {}};
    out = (stop_after_quote && source && source->poll_calls != 0) ||
          (service && stop_after_events > 0 &&
           service->delivered >= stop_after_events);
    return {};
  }
  base::Status Wait(std::optional<std::uint64_t> timeout) noexcept override {
    if (!timeout || waits.size() > 128) return {ErrorCode::kInternalError, {}};
    waits.push_back(*timeout);
    clock_.now += *timeout;
    return {};
  }

 private:
  TradingClock& clock_;
};

// Fully specified gateway-like snapshots; expected business values are owned
// by the fixture, independent of the production account engine.
testing::MockTradingService::Event Report(model::OrderUpdateKind kind,
                                          std::uint64_t sequence,
                                          std::int64_t cumulative,
                                          std::int64_t leaves,
                                          std::uint64_t deadline) {
  testing::MockTradingService::Event event;
  event.deadline = deadline;
  auto& dispatch = event.dispatch;
  dispatch.notify_order = true;
  dispatch.update.kind = kind;
  auto& order = dispatch.update.order;
  order.client_order_id = {1};
  EXPECT_EQ(model::MakeInstrumentId("SSE", "600000", order.instrument_id),
            ErrorCode::kOk);
  EXPECT_EQ(order.broker_order_id.Assign("mock-broker"), ErrorCode::kOk);
  order.side = model::OrderSide::kBuy;
  order.limit_price = 1001;
  order.original_qty = 200;
  order.cumulative_filled_qty = cumulative;
  order.leaves_qty = leaves;
  order.report_sequence = sequence;
  order.receive_time_ns = deadline;
  order.state =
      leaves == 0 ? model::OrderState::kFilled : model::OrderState::kWorking;
  if (kind == model::OrderUpdateKind::kFill) {
    dispatch.has_fill = true;
    auto& fill = dispatch.fill;
    fill.client_order_id = order.client_order_id;
    fill.broker_order_id = order.broker_order_id;
    fill.instrument_id = order.instrument_id;
    fill.side = order.side;
    fill.price = 1001;
    fill.quantity = 100;
    fill.receive_time_ns = deadline;
    EXPECT_EQ(fill.execution_id.Assign(sequence == 2 ? "fill-1" : "fill-2"),
              ErrorCode::kOk);
  }
  return event;
}

void ScriptFills(testing::MockTradingService& service) {
  service.initial_account.cash = {100000000000000, 0, 100000000000000};
  service.events = {Report(model::OrderUpdateKind::kAccepted, 1, 0, 200, 1000),
                    Report(model::OrderUpdateKind::kFill, 2, 100, 100, 2000),
                    Report(model::OrderUpdateKind::kFill, 3, 200, 0, 3000)};
}
StrategyOptions Options(const char* plugin = YQUANT_TEST_TRADING_NORMAL_PATH) {
  return {plugin, YQUANT_TEST_TRADING_STRATEGY_CONFIG_PATH,
          YQUANT_TEST_MARKET_DATA_CONFIG_PATH,
          YQUANT_TEST_TRADING_CLIENT_CONFIG_PATH};
}

TEST(TradingRunnerTest, UsesInjectedServiceAndDispatchesCommittedReports) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  ScriptFills(service);
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok()) << runner.error_detail();
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPSQNAVFHVFHfTD");
  EXPECT_EQ(service.send_calls, 1);
  EXPECT_EQ(service.delivered, 3);
  EXPECT_EQ(service.stop_calls, 1);
  EXPECT_EQ(source.stop_calls, 1);
  EXPECT_EQ(clock.now, 3000U);
}

TEST(TradingRunnerTest, ServiceInitializationFailurePreventsPluginCreation) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  service.init_result = {ErrorCode::kIoError, {}};
  TradingLoopEnvironment loop(clock);
  RunnerTestHarness runner(clock, loop, source, service);
  EXPECT_EQ(runner.Handle(Options()).code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "trading_connect");
  EXPECT_TRUE(events.Read().empty());
  EXPECT_EQ(service.init_calls, 1);
  EXPECT_EQ(service.stop_calls, 1);
  EXPECT_EQ(source.stop_calls, 1);
}

struct CallbackFailure {
  const char* name;
  const char* plugin;
  const char* stage;
  const char* trace;
};
class TradingServiceCallbackTest
    : public ::testing::TestWithParam<CallbackFailure> {};
TEST_P(TradingServiceCallbackTest, SuppressesCallbacksWhileServiceDrains) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  ScriptFills(service);
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options(GetParam().plugin)).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kInvalidArgument);
  EXPECT_STREQ(runner.error_stage(), GetParam().stage);
  EXPECT_EQ(events.Read(), GetParam().trace);
  EXPECT_EQ(service.delivered, 3);
  EXPECT_EQ(service.stop_calls, 1);
}
INSTANTIATE_TEST_SUITE_P(
    CallbackFailures, TradingServiceCallbackTest,
    ::testing::Values(CallbackFailure{"OrderUpdate",
                                      YQUANT_TEST_TRADING_UPDATE_FAIL_PATH,
                                      "order_update", "CPSQNAfTD"},
                      CallbackFailure{"Fill",
                                      YQUANT_TEST_TRADING_FILL_FAIL_PATH,
                                      "fill", "CPSQNAVFHfTD"}),
    [](const auto& info) { return info.param.name; });

TEST(TradingRunnerTest, UncertainServiceSendIsNotRetried) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  service.send_result = {ErrorCode::kIoError, {}};
  TradingLoopEnvironment loop(clock);
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "trading_send");
  EXPECT_EQ(service.send_calls, 1);
  EXPECT_EQ(service.stop_calls, 1);
  EXPECT_EQ(events.Read(), "CPSQNpTD");
}

TEST(TradingRunnerTest, PollFailurePreservesFirstDiagnosticAndFinalizes) {
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  ScriptFills(service);
  service.poll_result = {ErrorCode::kCorrupt, {}};
  service.stop_result = {ErrorCode::kIoError, {}};
  TradingLoopEnvironment loop(clock);
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kCorrupt);
  EXPECT_STREQ(runner.error_stage(), "trading_poll");
  EXPECT_EQ(service.delivered, 0);
  EXPECT_EQ(service.stop_calls, 1);
}

TEST(TradingRunnerTest, StopsBeforeSendingQueuedRequestsAndBoundsDispatch) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  loop.source = &source;
  loop.stop_after_quote = true;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options(YQUANT_TEST_TRADING_BULK_PATH)).ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  const auto trace = events.Read();
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'N'), 200);
  EXPECT_EQ(std::count(trace.begin(), trace.end(), 'B'), 200);
  EXPECT_EQ(service.send_calls, 0);
  EXPECT_GE(loop.checks, 6);
}

TEST(TradingRunnerTest, DrainTimeoutUsesClientShutdownDeadline) {
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  ScriptFills(service);
  service.never_drained = true;
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "trading_drain_timeout");
  EXPECT_EQ(clock.now, 1000003000U);
}

TEST(TradingRunnerTest, ReadyMarketDoesNotStarveServiceReports) {
  TradingClock clock;
  clock.now = 10000;
  TradingQuoteSource source;
  source.continuous = true;
  testing::MockTradingService service;
  ScriptFills(service);
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(service.delivered, 3);
  EXPECT_EQ(service.send_calls, 1);
  EXPECT_LE(source.poll_calls, 128);
}

TEST(TradingRunnerTest, MarketMaintenanceFailureStillDrainsService) {
  TradingClock clock;
  TradingQuoteSource source;
  source.service_deadline = 500;
  source.fail_service = true;
  testing::MockTradingService service;
  ScriptFills(service);
  TradingLoopEnvironment loop(clock);
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options(YQUANT_TEST_TRADING_FILL_FAIL_PATH)).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "market_data_service");
  EXPECT_EQ(service.delivered, 3);
  EXPECT_EQ(source.serviced_at, 500U);
  EXPECT_EQ(service.stop_calls, 1);
}

TEST(TradingRunnerTest, ReportsServiceStopFailureAfterSuccessfulCallbacks) {
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  ScriptFills(service);
  service.stop_result = {ErrorCode::kIoError, {}};
  TradingLoopEnvironment loop(clock);
  loop.service = &service;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(runner.Handle(Options()).ok());
  EXPECT_EQ(runner.Run().code, ErrorCode::kIoError);
  EXPECT_STREQ(runner.error_stage(), "trading_stop");
}

TEST(TradingRunnerTest, ServiceCapacityRejectionPreservesStrategyLifecycle) {
  TradingEventPipe events;
  ASSERT_TRUE(events.valid());
  TradingClock clock;
  TradingQuoteSource source;
  testing::MockTradingService service;
  service.submit_result = ErrorCode::kResourceExhausted;
  TradingLoopEnvironment loop(clock);
  loop.source = &source;
  loop.stop_after_quote = true;
  RunnerTestHarness runner(clock, loop, source, service);
  ASSERT_TRUE(
      runner.Handle(Options(YQUANT_TEST_TRADING_INSUFFICIENT_PATH)).ok());
  ASSERT_TRUE(runner.Run().ok()) << runner.error_detail();
  EXPECT_EQ(service.send_calls, 0);
  EXPECT_EQ(events.Read(), "CPSQLTD");
}
}  // namespace
}  // namespace yquant::strategyd
