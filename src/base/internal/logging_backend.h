#ifndef YQUANT_BASE_INTERNAL_LOGGING_BACKEND_H_
#define YQUANT_BASE_INTERNAL_LOGGING_BACKEND_H_

#include <sys/types.h>

#include <cstddef>
#include <span>

namespace yquant::base {

class Logger;

namespace internal {

// Tests set these hooks before Initialize() and retain the hook/context storage
// until every affected manager has shut down. A manager snapshots the pointer
// once; hooks run only on its backend thread. Null operations use POSIX calls.
struct LogBackendHooks {
  void* context = nullptr;
  void (*before_poll)(void*) noexcept = nullptr;
  // Runs after allocation and initialization, before publishing the Logger.
  void (*on_queue_ready)(void*, Logger*,
                         std::span<std::byte>) noexcept = nullptr;
  int (*open)(void*, const char*, int, mode_t) noexcept = nullptr;
  ssize_t (*write)(void*, int, const void*, std::size_t) noexcept = nullptr;
  int (*sync)(void*, int) noexcept = nullptr;
  int (*close)(void*, int) noexcept = nullptr;
  int (*rename)(void*, const char*, const char*) noexcept = nullptr;
};

void SetLogBackendHooksForTesting(const LogBackendHooks* hooks) noexcept;

}  // namespace internal
}  // namespace yquant::base

#endif  // YQUANT_BASE_INTERNAL_LOGGING_BACKEND_H_
