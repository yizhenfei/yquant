#ifndef YQUANT_TRADINGD_CONFIG_H_
#define YQUANT_TRADINGD_CONFIG_H_

#include <stddef.h>
#include <stdint.h>

#include "yquant/base/config_error.h"
#include "yquant/market/market_data_config.h"
#include "yquant/trading/protocol.h"
#include "yquant/trading/trading_config.h"

namespace yquant::tradingd {

inline constexpr uint64_t kHeartbeatIntervalNs = 250000000;
inline constexpr uint64_t kNanosecondsPerMillisecond = 1000000;
// A deadline spans at least four heartbeat intervals and at most one minute.
inline constexpr uint64_t kMinimumTimeoutMs = 1000;
inline constexpr uint64_t kMaximumTimeoutMs = 60000;
inline constexpr uint64_t kMinimumTimeoutNs =
    kMinimumTimeoutMs * kNanosecondsPerMillisecond;
inline constexpr uint64_t kMaximumTimeoutNs =
    kMaximumTimeoutMs * kNanosecondsPerMillisecond;

// Startup configuration for the account shared by all connected clients.
struct ServerConfig {
  trading::Endpoint listen;
  market::MarketDataConfig market_data;
  trading::TradingConfig trading;
  char token[trading::kMaxTokenBytes + 1] = {};
  size_t max_clients = trading::kMaxClients;
  uint64_t heartbeat_timeout_ns = 5000000000;
  uint64_t shutdown_timeout_ns = 5000000000;
};

// Validates typed values without acquiring sockets. Port zero is accepted for
// ephemeral test listeners; the JSON loader requires a nonzero port.
[[nodiscard]] base::Status ValidateConfig(const ServerConfig& config,
                                          base::ConfigError& error) noexcept;

class ConfigLoader {
 public:
  ConfigLoader() = default;
  ConfigLoader(const ConfigLoader&) = default;
  ConfigLoader& operator=(const ConfigLoader&) = default;

  // Strict JSON; relative paths use the process working directory.
  // Failure resets config() and retains an owned diagnostic until next Load.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const ServerConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  ServerConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::tradingd
#endif  // YQUANT_TRADINGD_CONFIG_H_
