#ifndef YQUANT_STRATEGYD_TRADING_CONFIG_H_
#define YQUANT_STRATEGYD_TRADING_CONFIG_H_

#include <cstdint>

#include "strategyd/trading_account.h"
#include "tradingd/protocol.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy_config.h"

namespace yquant::strategyd {

inline constexpr std::uint64_t kTradingNanosecondsPerMillisecond = 1000000;
inline constexpr std::uint64_t kMinimumTradingDurationMs = 1;
inline constexpr std::uint64_t kMaximumTradingDurationMs = 60000;
inline constexpr std::uint64_t kDefaultTradingTimeoutNs =
    5000 * kTradingNanosecondsPerMillisecond;

enum class MockTradingMode { kFill, kWorking, kReject };
enum class TradingAdapterKind { kMock, kTcp, kSimulatedFutures };

// An authenticated TCP endpoint. Credentials identify one configured strategy
// client and are never shared between concurrently running strategyd instances.
struct RemoteTradingConfig {
  tradingd::Endpoint endpoint;
  std::uint64_t client_id = 0;
  char token[tradingd::kMaxTokenBytes + 1] = {};
  std::uint64_t connect_timeout_ns = kDefaultTradingTimeoutNs;
  std::uint64_t heartbeat_timeout_ns = kDefaultTradingTimeoutNs;
};

// Owned initialization values; both intervals use the host's monotonic domain.
struct TradingConfig {
  TradingAdapterKind adapter = TradingAdapterKind::kMock;
  RemoteTradingConfig remote;
  MockTradingMode mode = MockTradingMode::kFill;
  std::uint64_t report_interval_ns = kTradingNanosecondsPerMillisecond;
  std::uint64_t shutdown_timeout_ns = kDefaultTradingTimeoutNs;
  TradingAccountSnapshot initial_account;
};

// Validates typed values too. Failure copies a field path/reason into error;
// config is never modified. Each duration must be within 1..60000 ms. Initial
// cash/quantities are nonnegative and positions are distinct. The adapter must
// match the account type; equity/futures bootstrap inventories cannot mix.
// Futures buckets are zero/zero or positive quantity/open_price pairs. The host
// checks dictionary membership and contract steps when configuring ledgers.
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
