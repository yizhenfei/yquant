#ifndef YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_
#define YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_

#include <cstddef>
#include <string_view>

#include "yquant/base/status.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxConfigFileBytes = 1024 * 1024;
inline constexpr std::size_t kMaxConfigDepth = 64;

// Owns validated source text until configuration initialization is complete.
// The JSON parser and duplicate-key scratch storage are local to Load().
class StrategyConfigLoader {
 public:
  StrategyConfigLoader() noexcept = default;
  ~StrategyConfigLoader() noexcept;

  StrategyConfigLoader(const StrategyConfigLoader&) = delete;
  StrategyConfigLoader& operator=(const StrategyConfigLoader&) = delete;
  StrategyConfigLoader(StrategyConfigLoader&&) = delete;
  StrategyConfigLoader& operator=(StrategyConfigLoader&&) = delete;

  [[nodiscard]] base::Status Load(const char* path) noexcept;
  // Invalidates parameters_json(). Diagnostics remain valid until Load().
  void Reset() noexcept;

  [[nodiscard]] const strategy::StrategyId& strategy_id() const noexcept {
    return strategy_id_;
  }
  [[nodiscard]] std::string_view parameters_json() const noexcept {
    return parameters_json_;
  }
  [[nodiscard]] const strategy::ConfigError& error() const noexcept {
    return error_;
  }

 private:
  char* source_ = nullptr;
  strategy::StrategyId strategy_id_;
  std::string_view parameters_json_;
  strategy::ConfigError error_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_
