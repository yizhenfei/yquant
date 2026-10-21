#ifndef YQUANT_TESTS_TEST_CONFIG_FILE_H_
#define YQUANT_TESTS_TEST_CONFIG_FILE_H_

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string_view>

namespace yquant::testing {

class ConfigFile {
 public:
  explicit ConfigFile(std::string_view contents) {
    const int fd = mkstemp(path_);
    if (fd < 0) {
      return;
    }
    std::size_t written = 0;
    while (written < contents.size()) {
      const ssize_t count =
          write(fd, contents.data() + written, contents.size() - written);
      if (count > 0) {
        written += static_cast<std::size_t>(count);
      } else if (count < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    valid_ = close(fd) == 0 && written == contents.size();
  }

  ~ConfigFile() { unlink(path_); }
  ConfigFile(const ConfigFile&) = delete;
  ConfigFile& operator=(const ConfigFile&) = delete;

  bool valid() const { return valid_; }
  const char* path() const { return path_; }

 private:
  char path_[64] = "/tmp/yquant-config-XXXXXX";
  bool valid_ = false;
};

}  // namespace yquant::testing

#endif  // YQUANT_TESTS_TEST_CONFIG_FILE_H_
