#ifndef YQUANT_BASE_PATH_H_
#define YQUANT_BASE_PATH_H_

#include <span>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::base {

// Writes an absolute, NUL-terminated path, resolving relative input against the
// process's current working directory. Does not check existence, canonicalize
// symlinks or remove dot components. Input may alias output.
// Empty/NUL-containing input or insufficient output returns kInvalidArgument;
// current-directory errors return kIoError with the system error. Failure
// preserves output. May allocate and query the filesystem; use during
// initialization and serialize against working-directory changes in other
// threads.
[[nodiscard]] Status ResolvePath(std::string_view path,
                                 std::span<char> out) noexcept;

}  // namespace yquant::base
#endif  // YQUANT_BASE_PATH_H_
