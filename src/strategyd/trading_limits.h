#ifndef YQUANT_STRATEGYD_TRADING_LIMITS_H_
#define YQUANT_STRATEGYD_TRADING_LIMITS_H_
#include <cstddef>
namespace yquant::strategyd {
inline constexpr std::size_t kMaxTradingOrders = 1024;
inline constexpr std::size_t kMaxTradingFills = 8192;
inline constexpr std::size_t kMaxTradingRequests = 256;
inline constexpr std::size_t kMaxLocalOrderUpdates = 256;
}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_TRADING_LIMITS_H_
