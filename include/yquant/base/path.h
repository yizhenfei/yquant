// Resolves and manipulates POSIX path spellings for configuration/startup.
// File creation, copying, and directory traversal belong in file.h.

#ifndef YQUANT_BASE_PATH_H_
#define YQUANT_BASE_PATH_H_

#include <span>
#include <string>
#include <string_view>

#include "yquant/base/status.h"

namespace yquant::base {

// Joins two path spellings; an absolute right operand replaces the left one.
std::string JoinPath(std::string_view left, std::string_view right);
// Returns the last component, or empty for a trailing slash or root.
std::string_view PathFilename(std::string_view path) noexcept;
// Returns the parent spelling, retaining root's slash.
std::string ParentPath(std::string_view path);
// Removes redundant separators and dot components without querying the disk.
std::string NormalizePath(std::string_view path);
// Writes the absolute working directory; failure preserves output.
[[nodiscard]] Status GetWorkingDirectory(std::string& output);
// Resolves existing prefixes through realpath, then appends missing components.
// Fails on permission/I/O errors. Failure preserves output.
[[nodiscard]] Status CanonicalizePath(std::string_view path,
                                      std::string& output);

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
