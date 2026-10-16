#include "strategyd/strategy_runtime.h"

#include <dlfcn.h>

#include <cerrno>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "gtest/gtest.h"
#include "yquant/base/status.h"

namespace yquant::strategyd {
namespace {

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

TEST(StrategyRuntimeTest, StartsStopsAndRejectsASecondLoad) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  StrategyRuntime runtime;
  EXPECT_EQ(runtime.state(), RuntimeState::kIdle);
  ASSERT_TRUE(runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).ok());
  EXPECT_EQ(runtime.state(), RuntimeState::kRunning);
  EXPECT_EQ(runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).code,
            base::ErrorCode::kInvalidState);
  EXPECT_EQ(runtime.state(), RuntimeState::kRunning);
  EXPECT_EQ(events.Read(), "CS");

  ASSERT_TRUE(runtime.Handle(Stop{}).ok());
  EXPECT_EQ(runtime.state(), RuntimeState::kFinished);
  ExpectLifecycle(events.Read(), "TD");
  EXPECT_TRUE(runtime.Handle(Stop{}).ok());
  EXPECT_EQ(runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).code,
            base::ErrorCode::kInvalidState);
  EXPECT_TRUE(events.Read().empty());
}

TEST(StrategyRuntimeTest, StopBeforeLoadFinishesWithoutLoading) {
  StrategyRuntime runtime;
  ASSERT_TRUE(runtime.Handle(Stop{}).ok());
  EXPECT_EQ(runtime.state(), RuntimeState::kFinished);
  EXPECT_TRUE(runtime.Handle(Stop{}).ok());
  EXPECT_EQ(runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).code,
            base::ErrorCode::kInvalidState);
}

TEST(StrategyRuntimeTest, DestructorStopsAnActiveStrategy) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  {
    StrategyRuntime runtime;
    ASSERT_TRUE(
        runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).ok());
  }
  ExpectLifecycle(events.Read(), "CSTD");
}

TEST(StrategyRuntimeTest, DestroysBeforeReleasingTheLibraryReference) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  // Calibrate finalizer observability without requiring physical unload on
  // platforms where the loader keeps this module resident.
  void* probe = dlopen(YQUANT_TEST_PLUGIN_GOOD_PATH, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(probe, nullptr);
  ASSERT_EQ(dlclose(probe), 0);
  const std::string probe_events = events.Read();
  EXPECT_TRUE(probe_events.empty() || probe_events == "U");

  StrategyRuntime runtime;
  ASSERT_TRUE(runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_GOOD_PATH}).ok());
  ASSERT_TRUE(runtime.Handle(Stop{}).ok());
  const std::string lifecycle = events.Read();
  ExpectLifecycle(lifecycle, "CSTD");
  if (probe_events == "U") {
    EXPECT_EQ(lifecycle, "CSTDU");
  }
}

TEST(StrategyRuntimeTest, RejectsEmptyPathsAndFinishes) {
  const char* paths[] = {nullptr, ""};
  for (const char* path : paths) {
    StrategyRuntime runtime;
    EXPECT_EQ(runtime.Handle(LoadAndStart{path}).code,
              base::ErrorCode::kInvalidArgument);
    EXPECT_EQ(runtime.state(), RuntimeState::kFinished);
    EXPECT_STREQ(runtime.error_stage(), "path");
    EXPECT_TRUE(runtime.Handle(Stop{}).ok());
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
  StrategyRuntime runtime;
  EXPECT_FALSE(runtime.Handle(LoadAndStart{GetParam().path}).ok());
  EXPECT_EQ(runtime.state(), RuntimeState::kFinished);
  ExpectLifecycle(events.Read(), GetParam().events);
  EXPECT_TRUE(runtime.Handle(Stop{}).ok());
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
        InvalidPlugin{"ShortDescriptor", YQUANT_TEST_PLUGIN_BAD_SIZE_PATH,
                      ""},
        InvalidPlugin{"MissingCreate", YQUANT_TEST_PLUGIN_NO_CREATE_PATH,
                      ""},
        InvalidPlugin{"MissingDestroy", YQUANT_TEST_PLUGIN_NO_DESTROY_PATH,
                      ""},
        InvalidPlugin{"CreateFailure", YQUANT_TEST_PLUGIN_CREATE_FAIL_PATH,
                      "C"},
        InvalidPlugin{"NullInstance", YQUANT_TEST_PLUGIN_CREATE_NULL_PATH,
                      "C"},
        InvalidPlugin{"InconsistentFactory",
                      YQUANT_TEST_PLUGIN_CREATE_INCONSISTENT_PATH, "CD"},
        InvalidPlugin{"StartFailure", YQUANT_TEST_PLUGIN_START_FAIL_PATH,
                      "CSD"}),
    [](const ::testing::TestParamInfo<InvalidPlugin>& info) {
      return info.param.name;
    });

TEST(StrategyRuntimeTest, StopFailureStillDestroysAndCloses) {
  EventPipe events;
  ASSERT_TRUE(events.valid());
  StrategyRuntime runtime;
  ASSERT_TRUE(
      runtime.Handle(LoadAndStart{YQUANT_TEST_PLUGIN_STOP_FAIL_PATH}).ok());
  EXPECT_EQ(runtime.Handle(Stop{}).code, base::ErrorCode::kInvalidArgument);
  EXPECT_STREQ(runtime.error_stage(), "stop");
  EXPECT_EQ(runtime.state(), RuntimeState::kFinished);
  ExpectLifecycle(events.Read(), "CSTD");
  EXPECT_TRUE(runtime.Handle(Stop{}).ok());
  EXPECT_TRUE(events.Read().empty());
}

}  // namespace
}  // namespace yquant::strategyd
