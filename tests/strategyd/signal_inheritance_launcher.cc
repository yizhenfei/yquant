#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

extern char** environ;

// Establish exec-inherited signal state without changing the test parent.
int main(int argc, char* argv[]) {
  if (argc < 3) {
    return 2;
  }
  if (std::strcmp(argv[1], "blocked") == 0) {
    sigset_t signals;
    if (sigemptyset(&signals) != 0 || sigaddset(&signals, SIGINT) != 0 ||
        sigaddset(&signals, SIGTERM) != 0 ||
        sigprocmask(SIG_BLOCK, &signals, nullptr) != 0) {
      return 1;
    }
  } else if (std::strcmp(argv[1], "ignored") == 0) {
    struct sigaction action{};
    action.sa_handler = SIG_IGN;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0) {
      return 1;
    }
  } else {
    return 2;
  }
  execve(argv[2], argv + 2, environ);
  std::perror("execve strategyd");
  return 1;
}
