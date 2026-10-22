#ifndef YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_
#define YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_

#include <cstddef>

#include "yquant/base/status.h"
#include "yquant/strategy/strategy_plugin.h"

namespace yquant::strategyd {

// Loads a strategy plugin and validates its ABI descriptor before exposing its
// factory functions. Owns one dynamic-loader reference, not the created
// strategy instances. Serialize all operations; destroy instances before Close
// or this object's destruction so their code remains loaded through cleanup.
class StrategyLibrary {
 public:
  StrategyLibrary() noexcept = default;
  ~StrategyLibrary() noexcept;

  StrategyLibrary(const StrategyLibrary&) = delete;
  StrategyLibrary& operator=(const StrategyLibrary&) = delete;
  StrategyLibrary(StrategyLibrary&&) = delete;
  StrategyLibrary& operator=(StrategyLibrary&&) = delete;

  // Opens a plugin using a filesystem path, resolving relative paths against
  // the working directory. Borrows path only during this call; null or empty
  // paths return kInvalidArgument. Failure leaves a closed object closed;
  // reopening returns kInvalidState without releasing the existing plugin.
  // Every call replaces previous diagnostics.
  [[nodiscard]] base::Status Open(const char* path) noexcept;
  // Releases the loader reference and clears plugin even if dlclose fails.
  // Already-closed calls succeed. Preserves preceding errors; otherwise records
  // a close error. All strategy instances must already be destroyed.
  [[nodiscard]] base::Status Close() noexcept;

  // Returns the validated descriptor while open, otherwise nullptr. The
  // descriptor and its function pointers must not be used after Close.
  [[nodiscard]] const strategy::StrategyPlugin* plugin() const noexcept {
    return plugin_;
  }
  // Returns non-null diagnostic strings, empty if no error has been recorded.
  // Contents are preserved by successful Close and replaced by the next Open.
  [[nodiscard]] const char* error_stage() const noexcept {
    return error_stage_;
  }
  // Returns owned, bounded detail text, which may be truncated.
  [[nodiscard]] const char* error_detail() const noexcept {
    return error_detail_;
  }

 private:
  // Records only the first error since Open. Requires non-null C strings;
  // stage must outlive the stored diagnostic, while detail is copied.
  void SetError(const char* stage, const char* detail) noexcept;

  // Diagnostic capacity in bytes, including the terminating null.
  static constexpr std::size_t kErrorDetailSize = 1024;
  void* handle_ = nullptr;  // Owned loader reference, null when closed.
  const strategy::StrategyPlugin* plugin_ = nullptr;  // Borrowed from handle_.
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_
