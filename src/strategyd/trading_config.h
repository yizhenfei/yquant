#ifndef YQUANT_STRATEGYD_TRADING_CONFIG_H_
#define YQUANT_STRATEGYD_TRADING_CONFIG_H_

#include <cstdint>

#include "strategyd/trading_account.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

enum class MockTradingMode { kFill, kWorking, kReject };

// Owned initialization values; both intervals use the host's monotonic domain.
struct TradingConfig {
  MockTradingMode mode = MockTradingMode::kFill;
  std::uint64_t report_interval_ns = 1000000;
  std::uint64_t shutdown_timeout_ns = 5000000000;
  TradingAccountSnapshot initial_account;
};

// Validates typed values too. Failure copies a field path/reason into error;
// config is never modified. Each duration must be within 1..60000 ms. Initial
// cash/quantities are nonnegative, positions are distinct, and reserved is
// zero.
[[nodiscard]] base::Status ValidateTradingConfig(
    const TradingConfig& config, strategy::ConfigError& error) noexcept;

// Main-thread strict JSON loader. Retains only typed values and diagnostics.
class TradingConfigLoader {
 public:
  // Replaces previous values; failure resets config() to defaults and owns its
  // diagnostic until the next Load. path is borrowed only during this call.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const TradingConfig& config() const noexcept { return config_; }
  const strategy::ConfigError& error() const noexcept { return error_; }

 private:
  TradingConfig config_;
  strategy::ConfigError error_;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TRADING_CONFIG_H_
