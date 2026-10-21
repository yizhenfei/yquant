#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <type_traits>

#include "gtest/gtest.h"
#include "runner_test_harness.h"
#include "test_config_file.h"
#include "yquant/base/status.h"

namespace yquant::strategyd {
namespace {

static_assert(!std::is_default_constructible_v<StrategyRunner>);
static_assert(
    std::is_constructible_v<StrategyRunner, const base::MonotonicClock&,
                            base::EventLoopEnvironment&, MarketDataSource&,
                            trading::TradingService&>);

TEST(StrategyAssemblyTest, RequiresMarketConfiguration) {
  base::SteadyMonotonicClock clock;
  IdleTestEnvironment environment;
  StrategyAssembly assembly(clock);
  EXPECT_EQ(assembly
                .Load({YQUANT_TEST_PLUGIN_GOOD_PATH, YQUANT_TEST_CONFIG_PATH},
                      environment)
                .code,
            base::ErrorCode::kInvalidArgument);
  EXPECT_STREQ(assembly.error_stage(), "market_data_config");
}

TEST(StrategyAssemblyTest, RequiresGatewayAndRejectsBrokerConfiguration) {
  base::SteadyMonotonicClock clock;
  IdleTestEnvironment environment;
  for (const char* path :
       {static_cast<const char*>(nullptr), YQUANT_TEST_TRADING_CONFIG_PATH}) {
    StrategyAssembly assembly(clock);
    EXPECT_EQ(assembly
                  .Load({YQUANT_TEST_PLUGIN_GOOD_PATH, YQUANT_TEST_CONFIG_PATH,
                         YQUANT_TEST_MARKET_DATA_CONFIG_PATH, path},
                        environment)
                  .code,
              base::ErrorCode::kInvalidArgument);
    EXPECT_STREQ(assembly.error_stage(), "trading_config");
  }
}

class EventPipe {
 public:
  EventPipe() {
    const char* previous = std::getenv("YQUANT_TEST_EVENT_FD");
    had_previous_ = previous != nullptr;
    if (had_previous_) {
      previous_ = previous;
    }
    if (pipe(fds_) != 0) {
      return;
    }
    if (fcntl(fds_[0], F_SETFL, O_NONBLOCK) == -1) {
      return;
    }
    const std::string fd = std::to_string(fds_[1]);
    valid_ = setenv("YQUANT_TEST_EVENT_FD", fd.c_str(), 1) == 0;
  }

  ~EventPipe() {
    if (had_previous_) {
      setenv("YQUANT_TEST_EVENT_FD", previous_.c_str(), 1);
    } else {
      unsetenv("YQUANT_TEST_EVENT_FD");
    }
    for (int fd : fds_) {
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
        events.append(buffer, static_cast<size_t>(count));
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

void ExpectLifecycle(const std::string& events, const std::string& expected) {
  // A loader may retain a physical mapping. If its finalizer runs, it must run
  // after the strategy has been destroyed.
  const size_t unload = events.find('U');
  EXPECT_EQ(events.substr(0, unload), expected);
  if (unload != std::string::npos && expected.find('D') != std::string::npos) {
    EXPECT_LT(events.find('D'), unload);
  }
}

// Requests stop and drives finalization without exposing internal dispatch.
base::Status StopRunner(RunnerTestHarness& runner) noexcept {
  const auto requested = runner.Handle(Stop{});
  if (!requested.ok() || runner.state() == RunnerState::kFinished) {
    return requested;
  }
  return runner.Run();
}

TEST(StrategyRunnerTest, StartsStopsAndRejectsASecondLoad) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  RunnerTestHarness runner;
  EXPECT_EQ(runner.state(), RunnerState::kIdle);
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                          YQUANT_TEST_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(runner.state(), RunnerState::kRunning);
  EXPECT_EQ(runner.strategy_id(), "test-lifecycle");
  EXPECT_EQ(runner
                .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                        YQUANT_TEST_CONFIG_PATH})
                .code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(runner.state(), RunnerState::kRunning);
  EXPECT_EQ(events.Read(), "CPS");

  ASSERT_TRUE(StopRunner(runner).ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_EQ(runner.strategy_id(), "test-lifecycle");
  ExpectLifecycle(events.Read(), "TD");
  EXPECT_TRUE(StopRunner(runner).ok());
  EXPECT_EQ(runner
                .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                        YQUANT_TEST_CONFIG_PATH})
                .code,
            base::ErrorCode::kInvalidState);
  EXPECT_TRUE(events.Read().empty());
}

TEST(StrategyRunnerTest, StopBeforeLoadFinishesWithoutLoading) {
  RunnerTestHarness runner;
  ASSERT_TRUE(StopRunner(runner).ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_TRUE(StopRunner(runner).ok());
  EXPECT_EQ(runner
                .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                        YQUANT_TEST_CONFIG_PATH})
                .code,
            base::ErrorCode::kInvalidState);
}

TEST(StrategyRunnerTest, DestructorStopsAnActiveStrategy) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  {
    RunnerTestHarness runner;
    ASSERT_TRUE(runner
                    .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                            YQUANT_TEST_CONFIG_PATH})
                    .ok());
  }
  ExpectLifecycle(events.Read(), "CPSTD");
}

TEST(StrategyRunnerTest, DestroysBeforeReleasingTheLibraryReference) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  // Calibrate finalizer observability without requiring physical unload on
  // platforms where the loader keeps this module resident.
  void* probe = dlopen(YQUANT_TEST_PLUGIN_GOOD_PATH, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(probe, nullptr);
  ASSERT_EQ(dlclose(probe), 0);
  const std::string probe_events = events.Read();
  EXPECT_TRUE(probe_events.empty() || probe_events == "U");

  RunnerTestHarness runner;
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH,
                                          YQUANT_TEST_CONFIG_PATH})
                  .ok());
  ASSERT_TRUE(StopRunner(runner).ok());
  const std::string lifecycle = events.Read();
  ExpectLifecycle(lifecycle, "CPSTD");
  if (probe_events == "U") {
    EXPECT_EQ(lifecycle, "CPSTDU");
  }
}

TEST(StrategyRunnerTest, RejectsEmptyPathsAndFinishes) {
  const char* paths[] = {nullptr, ""};
  for (const char* path : paths) {
    RunnerTestHarness runner;
    EXPECT_EQ(
        runner.Handle(StrategyOptions{path, YQUANT_TEST_CONFIG_PATH}).code,
        base::ErrorCode::kInvalidArgument);
    EXPECT_EQ(runner.state(), RunnerState::kFinished);
    EXPECT_STREQ(runner.error_stage(), "path");
    EXPECT_TRUE(StopRunner(runner).ok());
  }
}

struct InvalidPlugin {
  const char* name;
  const char* path;
  const char* events;
};

class InvalidPluginTest : public ::testing::TestWithParam<InvalidPlugin> {};

TEST_P(InvalidPluginTest, FailsAndReleasesOwnedResources) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  RunnerTestHarness runner;
  EXPECT_FALSE(
      runner.Handle(StrategyOptions{GetParam().path, YQUANT_TEST_CONFIG_PATH})
          .ok());
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  ExpectLifecycle(events.Read(), GetParam().events);
  EXPECT_TRUE(StopRunner(runner).ok());
  EXPECT_TRUE(events.Read().empty());
}

INSTANTIATE_TEST_SUITE_P(
    DynamicLibraries, InvalidPluginTest,
    ::testing::Values(
        InvalidPlugin{"MissingLibrary", "/yquant-no-such-strategy.so", ""},
        InvalidPlugin{"NotDynamicLibrary", "/dev/null", ""},
        InvalidPlugin{"MissingEntry", YQUANT_TEST_PLUGIN_NO_ENTRY_PATH, ""},
        InvalidPlugin{"NullDescriptor", YQUANT_TEST_PLUGIN_NULL_PLUGIN_PATH,
                      ""},
        InvalidPlugin{"WrongAbi", YQUANT_TEST_PLUGIN_BAD_ABI_PATH, ""},
        InvalidPlugin{"ShortDescriptor", YQUANT_TEST_PLUGIN_BAD_SIZE_PATH, ""},
        InvalidPlugin{"MissingCreate", YQUANT_TEST_PLUGIN_NO_CREATE_PATH, ""},
        InvalidPlugin{"MissingDestroy", YQUANT_TEST_PLUGIN_NO_DESTROY_PATH, ""},
        InvalidPlugin{"CreateFailure", YQUANT_TEST_PLUGIN_CREATE_FAIL_PATH,
                      "C"},
        InvalidPlugin{"NullInstance", YQUANT_TEST_PLUGIN_CREATE_NULL_PATH, "C"},
        InvalidPlugin{"InconsistentFactory",
                      YQUANT_TEST_PLUGIN_CREATE_INCONSISTENT_PATH, "CD"},
        InvalidPlugin{"StartFailure", YQUANT_TEST_PLUGIN_START_FAIL_PATH,
                      "CPSD"}),
    [](const ::testing::TestParamInfo<InvalidPlugin>& info) {
      return info.param.name;
    });

TEST(StrategyRunnerTest, StopFailureStillDestroysAndCloses) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  RunnerTestHarness runner;
  ASSERT_TRUE(runner
                  .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_STOP_FAIL_PATH,
                                          YQUANT_TEST_CONFIG_PATH})
                  .ok());
  EXPECT_EQ(StopRunner(runner).code, base::ErrorCode::kInvalidArgument);
  EXPECT_STREQ(runner.error_stage(), "stop");
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  ExpectLifecycle(events.Read(), "CPSTD");
  EXPECT_TRUE(StopRunner(runner).ok());
  EXPECT_TRUE(events.Read().empty());
}

TEST(StrategyRunnerTest, RejectsConfigurationBeforeLoadingTheLibrary) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  testing::ConfigFile config(R"({"strategy_id":"x","parameters":[]})");
  ASSERT_TRUE(config.valid());
  RunnerTestHarness runner;
  EXPECT_FALSE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH, config.path()})
          .ok());
  EXPECT_STREQ(runner.error_stage(), "config");
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_TRUE(events.Read().empty());
  EXPECT_TRUE(StopRunner(runner).ok());
}

TEST(StrategyRunnerTest, MissingConfigPathsNeverCreateTheStrategy) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  for (const char* path :
       {static_cast<const char*>(nullptr), "", "/yquant-no-such-config.json"}) {
    RunnerTestHarness runner;
    EXPECT_FALSE(
        runner.Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH, path})
            .ok());
    EXPECT_EQ(runner.state(), RunnerState::kFinished);
    EXPECT_TRUE(events.Read().empty());
  }
}

TEST(StrategyRunnerTest, InitializesTypedParametersBeforeStartAndOwnsValues) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  testing::ConfigFile config(
      R"({"strategy_id":"typed","parameters":{"quote_qty":7,)"
      R"("label":"owned \u006cabel","large_value":9007199254740993}})");
  ASSERT_TRUE(config.valid());
  RunnerTestHarness runner;
  ASSERT_TRUE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_TYPED_PATH, config.path()})
          .ok())
      << runner.error_detail();
  EXPECT_EQ(events.Read(), "CPS");
  EXPECT_EQ(runner.state(), RunnerState::kRunning);
  ASSERT_TRUE(StopRunner(runner).ok());
  ExpectLifecycle(events.Read(), "TD");
}

struct InvalidParameters {
  const char* name;
  const char* parameters;
  const char* error_field;
};

class InvalidParametersTest
    : public ::testing::TestWithParam<InvalidParameters> {};

// Each CTest case loads one strategy, matching the production process.
TEST_P(InvalidParametersTest, DestroysWithoutStartOrStop) {
  const auto& test_case = GetParam();
  EventPipe events;
  ASSERT_TRUE(events.valid());
  const std::string json =
      std::string(R"({"strategy_id":"typed","parameters":)") +
      test_case.parameters + '}';
  testing::ConfigFile config(json);
  ASSERT_TRUE(config.valid());
  RunnerTestHarness runner;
  EXPECT_EQ(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_TYPED_PATH, config.path()})
          .code,
      base::ErrorCode::kInvalidArgument);
  EXPECT_EQ(runner.state(), RunnerState::kFinished);
  EXPECT_STREQ(runner.error_stage(), "initialize");
  EXPECT_EQ(runner.strategy_id(), "typed");
  EXPECT_NE(std::string(runner.error_detail())
                .find(std::string("parameters.") + test_case.error_field),
            std::string::npos);
  ExpectLifecycle(events.Read(), "CPD");
  EXPECT_TRUE(StopRunner(runner).ok());
  EXPECT_EQ(runner.strategy_id(), "typed");
  EXPECT_TRUE(events.Read().empty());
}

INSTANTIATE_TEST_SUITE_P(
    ParameterFailures, InvalidParametersTest,
    ::testing::Values(
        InvalidParameters{
            "MissingQuantity",
            R"({"label":"owned label","large_value":9007199254740993})",
            "quote_qty"},
        InvalidParameters{"MissingLabel",
                          R"({"quote_qty":1,"large_value":9007199254740993})",
                          "label"},
        InvalidParameters{"MissingLargeValue",
                          R"({"quote_qty":1,"label":"owned label"})",
                          "large_value"},
        InvalidParameters{"QuantityString",
                          R"({"quote_qty":"1","label":"owned label",)"
                          R"("large_value":9007199254740993})",
                          "quote_qty"},
        InvalidParameters{"QuantityFraction",
                          R"({"quote_qty":1.5,"label":"owned label",)"
                          R"("large_value":9007199254740993})",
                          "quote_qty"},
        InvalidParameters{"QuantityZero",
                          R"({"quote_qty":0,"label":"owned label",)"
                          R"("large_value":9007199254740993})",
                          "quote_qty"},
        InvalidParameters{"QuantityAboveLimit",
                          R"({"quote_qty":101,"label":"owned label",)"
                          R"("large_value":9007199254740993})",
                          "quote_qty"},
        InvalidParameters{
            "LabelNumber",
            R"({"quote_qty":1,"label":7,"large_value":9007199254740993})",
            "label"},
        InvalidParameters{"IntegerOverflow",
                          R"({"quote_qty":1,"label":"owned label",)"
                          R"("large_value":9223372036854775808})",
                          "large_value"},
        InvalidParameters{"UnknownParameter",
                          R"({"quote_qty":1,"label":"owned label",)"
                          R"("large_value":9007199254740993,"extra":1})",
                          "extra"}),
    [](const ::testing::TestParamInfo<InvalidParameters>& info) {
      return info.param.name;
    });

TEST(StrategyRunnerTest, DefaultConfigRejectsUnexpectedParameters) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  testing::ConfigFile config(
      R"({"strategy_id":"plain","parameters":{"unexpected":1}})");
  ASSERT_TRUE(config.valid());
  RunnerTestHarness runner;
  EXPECT_FALSE(
      runner
          .Handle(StrategyOptions{YQUANT_TEST_PLUGIN_GOOD_PATH, config.path()})
          .ok());
  EXPECT_STREQ(runner.error_stage(), "initialize");
  ExpectLifecycle(events.Read(), "CPD");
}

}  // namespace
}  // namespace yquant::strategyd
