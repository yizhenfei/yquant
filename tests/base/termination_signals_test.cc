#include "yquant/base/termination_signals.h"

#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <optional>

#include "gtest/gtest.h"
#include "yquant/base/event_loop_environment.h"

namespace yquant::base {
namespace {

// Waits for an isolated signal test; retries interruption of the parent's wait.
void ExpectSuccessfulChild(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  ASSERT_EQ(result, child);
  ASSERT_TRUE(WIFEXITED(status)) << "child status=" << status;
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(TerminationSignalsTest, RequiresPreparationAndPreservesStopOutput) {
  TerminationSignals signals;
  EventLoopEnvironment& environment = signals;
  bool stop = true;
  EXPECT_EQ(environment.CheckStop(stop).code, ErrorCode::kInvalidState);
  EXPECT_TRUE(stop);
  EXPECT_EQ(environment.Wait(0).code, ErrorCode::kInvalidState);
  EXPECT_EQ(environment.Wait(std::nullopt).code, ErrorCode::kInvalidState);
}

class TerminationSignalWaitTest : public ::testing::TestWithParam<int> {};

TEST_P(TerminationSignalWaitTest, PendingStopWakesWaitAndRestoresCallingMask) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    // Bound a lost-wakeup regression without timing-based readiness guesses.
    alarm(3);
    TerminationSignals signals;
    if (!signals.Prepare().ok()) {
      _exit(1);
    }
    sigset_t unrelated;
    if (sigemptyset(&unrelated) != 0 || sigaddset(&unrelated, SIGUSR1) != 0 ||
        pthread_sigmask(SIG_BLOCK, &unrelated, nullptr) != 0) {
      _exit(2);
    }
    if (raise(GetParam()) != 0) {
      _exit(3);
    }
    EventLoopEnvironment& environment = signals;
    if (!environment.Wait(std::nullopt).ok()) {
      _exit(4);
    }
    sigset_t restored;
    if (pthread_sigmask(SIG_SETMASK, nullptr, &restored) != 0 ||
        sigismember(&restored, SIGINT) != 1 ||
        sigismember(&restored, SIGTERM) != 1 ||
        sigismember(&restored, SIGUSR1) != 1) {
      _exit(5);
    }
    bool stop = false;
    if (!environment.CheckStop(stop).ok() || !stop) {
      _exit(6);
    }
    _exit(0);
  }
  ExpectSuccessfulChild(child);
}

INSTANTIATE_TEST_SUITE_P(ExitSignals, TerminationSignalWaitTest,
                         ::testing::Values(SIGINT, SIGTERM));

TEST(TerminationSignalsTest, TimeoutsDoNotRequestStopOrChangeTheCallingMask) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    alarm(3);
    TerminationSignals signals;
    if (!signals.Prepare().ok()) {
      _exit(1);
    }
    EventLoopEnvironment& environment = signals;
    if (!environment.Wait(0).ok() || !environment.Wait(1).ok()) {
      _exit(2);
    }
    sigset_t mask;
    if (pthread_sigmask(SIG_SETMASK, nullptr, &mask) != 0 ||
        sigismember(&mask, SIGINT) != 1 || sigismember(&mask, SIGTERM) != 1) {
      _exit(3);
    }
    bool stop = true;
    if (!environment.CheckStop(stop).ok() || stop) {
      _exit(4);
    }
    _exit(0);
  }
  ExpectSuccessfulChild(child);
}

}  // namespace
}  // namespace yquant::base
