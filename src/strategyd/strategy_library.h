#ifndef YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_
#define YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_

#include <cstddef>

#include "yquant/base/status.h"
#include "yquant/strategy/strategy_plugin.h"

namespace yquant::strategyd {

// Owns one dlopen reference. Instances must be destroyed before Close().
class StrategyLibrary {
 public:
  StrategyLibrary() noexcept = default;
  ~StrategyLibrary() noexcept;

  StrategyLibrary(const StrategyLibrary&) = delete;
  StrategyLibrary& operator=(const StrategyLibrary&) = delete;
  StrategyLibrary(StrategyLibrary&&) = delete;
  StrategyLibrary& operator=(StrategyLibrary&&) = delete;

  [[nodiscard]] base::Status Open(const char* path) noexcept;
  [[nodiscard]] base::Status Close() noexcept;

  [[nodiscard]] const strategy::StrategyPlugin* plugin() const noexcept {
    return plugin_;
  }
  [[nodiscard]] const char* error_stage() const noexcept {
    return error_stage_;
  }
  [[nodiscard]] const char* error_detail() const noexcept {
    return error_detail_;
  }

 private:
  void SetError(const char* stage, const char* detail) noexcept;

  static constexpr std::size_t kErrorDetailSize = 1024;
  void* handle_ = nullptr;
  const strategy::StrategyPlugin* plugin_ = nullptr;
  const char* error_stage_ = "";
  char error_detail_[kErrorDetailSize] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_LIBRARY_H_
