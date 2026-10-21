#ifndef YQUANT_TESTS_SCOPED_WORKING_DIRECTORY_H_
#define YQUANT_TESTS_SCOPED_WORKING_DIRECTORY_H_

#include <fcntl.h>
#include <unistd.h>

#include "gtest/gtest.h"

namespace yquant::testing {
// Process-wide cwd guard for serialized tests; create before starting threads
// or child processes and destroy before deleting the temporary directory.
class ScopedWorkingDirectory {
 public:
  explicit ScopedWorkingDirectory(const char* path)
      : original_(::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
    valid_ = original_ >= 0 && ::chdir(path) == 0;
  }
  ~ScopedWorkingDirectory() {
    if (original_ >= 0) {
      if (valid_) EXPECT_EQ(::fchdir(original_), 0);
      ::close(original_);
    }
  }
  ScopedWorkingDirectory(const ScopedWorkingDirectory&) = delete;
  ScopedWorkingDirectory& operator=(const ScopedWorkingDirectory&) = delete;
  bool valid() const { return valid_; }

 private:
  int original_ = -1;
  bool valid_ = false;
};
}  // namespace yquant::testing
#endif  // YQUANT_TESTS_SCOPED_WORKING_DIRECTORY_H_
