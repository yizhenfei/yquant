#ifndef YQUANT_TRADING_TRADING_LIMITS_H_
#define YQUANT_TRADING_TRADING_LIMITS_H_
#include <cstddef>
#include <cstdint>
namespace yquant::trading {
inline constexpr std::uint64_t kTradingNanosecondsPerMillisecond = 1000000;
inline constexpr std::uint64_t kMinimumTradingDurationMs = 1;
inline constexpr std::uint64_t kMaximumTradingDurationMs = 60000;
inline constexpr std::uint64_t kDefaultTradingTimeoutNs =
    5000 * kTradingNanosecondsPerMillisecond;

inline constexpr std::size_t kMaxTradingOrders = 1024;
inline constexpr std::size_t kMaxTradingFills = 8192;
inline constexpr std::size_t kMaxTradingRequests = 256;
inline constexpr std::size_t kMaxLocalOrderUpdates = 256;
}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_LIMITS_H_
