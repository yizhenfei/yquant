#ifndef YQUANT_STRATEGYD_TRADING_ACCOUNT_H_
#define YQUANT_STRATEGYD_TRADING_ACCOUNT_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "strategyd/market_data_config.h"
#include "yquant/model/account.h"

namespace yquant::strategyd {

// Owned bootstrap snapshot read before sending any requests. Only the first
// position_count elements are active; omitted known instruments start at zero.
// Initial reservations must be zero; the host creates its own reservations.
struct TradingAccountSnapshot {
  std::int64_t cash_balance = 0;
  std::array<model::PositionSnapshot, kMaxMarketDataInstruments> positions = {};
  std::size_t position_count = 0;
  friend bool operator==(const TradingAccountSnapshot&,
                         const TradingAccountSnapshot&) noexcept = default;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TRADING_ACCOUNT_H_
