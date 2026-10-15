#ifndef YQUANT_BASE_STATUS_H_
#define YQUANT_BASE_STATUS_H_

#include <cstdint>
#include <system_error>

namespace yquant::base {

enum class ErrorCode : std::uint8_t {
  kOk,
  kInvalidArgument,
  kInvalidState,
  kResourceExhausted,
  kUnsupported,
  kIoError,
  kCorrupt,
  kInternalError,
};

struct [[nodiscard]] Status {
  ErrorCode code = ErrorCode::kOk;
  std::error_code system_error;

  [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::kOk; }
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_STATUS_H_
