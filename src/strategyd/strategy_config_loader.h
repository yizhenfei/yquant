#ifndef YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_
#define YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_

#include <cstddef>
#include <string_view>

#include "yquant/base/json_config_document.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

// Loads the strategy ID and raw parameters object for plugin initialization.
// Owns the source behind parameters_json; the runner releases it after the
// plugin has copied its typed configuration. Serialize Load, Reset, and view
// access on the owning thread.
class StrategyConfigLoader {
 public:
  StrategyConfigLoader() noexcept = default;
  ~StrategyConfigLoader() noexcept;

  StrategyConfigLoader(const StrategyConfigLoader&) = delete;
  StrategyConfigLoader& operator=(const StrategyConfigLoader&) = delete;
  StrategyConfigLoader(StrategyConfigLoader&&) = delete;
  StrategyConfigLoader& operator=(StrategyConfigLoader&&) = delete;

  // Replaces the previous document and validates the root schema. Borrows path
  // only for this call; null or empty paths return kInvalidArgument. On failure
  // the ID and parameter view are empty and error owns the diagnostic. A later
  // Load replaces the diagnostic even if it succeeds.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  // Releases source storage and clears the ID and parameter view. Invalidates
  // outstanding JSON views while preserving the diagnostic.
  void Reset() noexcept;

  // Returns the owned ID, empty before loading or after Reset or a failed Load.
  [[nodiscard]] const strategy::StrategyId& strategy_id() const noexcept {
    return strategy_id_;
  }
  // Returns a JSON view valid until the next Load, Reset, or destruction.
  [[nodiscard]] std::string_view parameters_json() const noexcept {
    return parameters_json_;
  }
  [[nodiscard]] const base::ConfigError& error() const noexcept {
    return error_;
  }

 private:
  base::JsonConfigDocument document_;
  strategy::StrategyId strategy_id_;
  // Points into document_, never into the temporary schema parser.
  std::string_view parameters_json_;
  base::ConfigError error_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_STRATEGY_CONFIG_LOADER_H_
