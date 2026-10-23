#ifndef YQUANT_TRADING_TRADING_CONFIG_H_
#define YQUANT_TRADING_TRADING_CONFIG_H_

#include <stdint.h>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"
#include "yquant/trading/protocol.h"
#include "yquant/trading/trading_account.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {

enum class MockTradingMode { kFill, kWorking, kReject };
enum class TradingAdapterKind { kMock, kSimulatedFutures, kCtp };

// Local authenticated IPC to the SimNow SDK bridge. Credentials for the
// external trading account remain in the bridge process's environment.
struct CtpTradingConfig {
  char address[16] = "127.0.0.1";
  uint16_t port = 0;
  char token[kMaxTokenBytes + 1] = {};
  // Account initialization runs rate-limited SDK queries on the cold path.
  uint64_t connect_timeout_ns =
      kMaximumTradingDurationMs * kTradingNanosecondsPerMillisecond;
  uint64_t heartbeat_timeout_ns = kDefaultTradingTimeoutNs;
};

// Gateway-owned broker/account configuration. Strategy clients use the separate
// TradingClientConfig and never load these initial balances or positions.
struct TradingConfig {
  TradingAdapterKind adapter = TradingAdapterKind::kMock;
  MockTradingMode mode = MockTradingMode::kFill;
  uint64_t report_interval_ns = kTradingNanosecondsPerMillisecond;
  uint64_t shutdown_timeout_ns = kDefaultTradingTimeoutNs;
  TradingAccountSnapshot initial_account;
  CtpTradingConfig ctp;
};

// Validates typed values too. Failure copies a field path/reason into error;
// config is never modified. Each duration must be within 1..60000 ms. Initial
// cash/quantities are nonnegative, positions are distinct, and reserved is
// zero.
[[nodiscard]] base::Status ValidateTradingConfig(
    const TradingConfig& config, base::ConfigError& error) noexcept;

// Main-thread strict JSON loader. Retains only typed values and diagnostics.
class TradingConfigLoader {
 public:
  TradingConfigLoader() = default;
  TradingConfigLoader(const TradingConfigLoader&) = default;
  TradingConfigLoader& operator=(const TradingConfigLoader&) = default;

  // Replaces previous values; failure resets config() to defaults and owns its
  // diagnostic until the next Load. path is borrowed only during this call.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const TradingConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  TradingConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_CONFIG_H_
