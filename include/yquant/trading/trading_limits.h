#ifndef YQUANT_TRADING_TRADING_LIMITS_H_
#define YQUANT_TRADING_TRADING_LIMITS_H_
#include <cstddef>
namespace yquant::trading {
inline constexpr std::size_t kMaxTradingOrders = 1024;
inline constexpr std::size_t kMaxTradingFills = 8192;
inline constexpr std::size_t kMaxTradingRequests = 256;
inline constexpr std::size_t kMaxLocalOrderUpdates = 256;
}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_LIMITS_H_
