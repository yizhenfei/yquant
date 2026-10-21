#ifndef YQUANT_TRADING_TRADING_ACCOUNT_H_
#define YQUANT_TRADING_TRADING_ACCOUNT_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "yquant/md/market_data_config.h"
#include "yquant/model/account.h"

namespace yquant::trading {

// Owned bootstrap snapshot read before sending any requests. Only the first
// position_count elements are active; omitted known instruments start at zero.
// Initial reservations must be zero; the host creates its own reservations.
struct TradingAccountSnapshot {
  std::int64_t cash_balance = 0;
  std::array<model::PositionSnapshot, md::kMaxMarketDataInstruments> positions =
      {};
  std::size_t position_count = 0;
  friend bool operator==(const TradingAccountSnapshot&,
                         const TradingAccountSnapshot&) noexcept = default;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ACCOUNT_H_
