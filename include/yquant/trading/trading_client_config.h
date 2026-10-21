#ifndef YQUANT_TRADING_TRADING_CLIENT_CONFIG_H_
#define YQUANT_TRADING_TRADING_CLIENT_CONFIG_H_

#include "yquant/base/config_error.h"
#include "yquant/trading/protocol.h"
#include "yquant/trading/trading_limits.h"

namespace yquant::trading {

// Connection and identity for one strategy's tradingd session. Accounts and
// broker configuration belong to tradingd; the client never bootstraps them.
struct TradingClientConfig {
  Endpoint endpoint;
  std::uint64_t client_id = 0;
  char token[kMaxTokenBytes + 1] = {};
  std::uint64_t connect_timeout_ns = kDefaultTradingTimeoutNs;
  std::uint64_t heartbeat_timeout_ns = kDefaultTradingTimeoutNs;
  std::uint64_t shutdown_timeout_ns = kDefaultTradingTimeoutNs;
};

// Requires a nonzero numeric IPv4 endpoint/identity, a printable ASCII token,
// and timeouts within 1..60000 ms. Failure owns a field diagnostic.
[[nodiscard]] base::Status ValidateTradingClientConfig(
    const TradingClientConfig& config, base::ConfigError& error) noexcept;

// Loads strict TCP client JSON. Rejects broker/initial-account fields. Failure
// resets config() and retains owned diagnostics until the next Load.
class TradingClientConfigLoader {
 public:
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const TradingClientConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  TradingClientConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_CLIENT_CONFIG_H_
