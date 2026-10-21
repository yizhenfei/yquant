#ifndef YQUANT_TRADING_TRADING_ACCOUNT_H_
#define YQUANT_TRADING_TRADING_ACCOUNT_H_

#include <array>
#include <cstddef>
#include <cstdint>

#include "yquant/md/market_data_config.h"
#include "yquant/model/account.h"
#include "yquant/model/futures.h"

namespace yquant::trading {

struct FuturesInitialBucket {
  std::int64_t quantity = 0;
  std::int64_t open_price = 0;
  friend bool operator==(const FuturesInitialBucket&,
                         const FuturesInitialBucket&) noexcept = default;
};

// Each nonempty bootstrap bucket becomes one exact FIFO opening lot.
// reference_price initializes both sides of the conservative mark.
struct FuturesInitialPosition {
  model::InstrumentId instrument_id;
  FuturesInitialBucket long_today;
  FuturesInitialBucket long_yesterday;
  FuturesInitialBucket short_today;
  FuturesInitialBucket short_yesterday;
  std::int64_t reference_price = 0;
  friend bool operator==(const FuturesInitialPosition&,
                         const FuturesInitialPosition&) noexcept = default;
};

// Owned bootstrap snapshot read before sending any requests. Only the first
// position_count elements are active; omitted known instruments start at zero.
// Initial reservations must be zero; the host creates its own reservations.
struct TradingAccountSnapshot {
  std::int64_t cash_balance = 0;
  std::array<model::PositionSnapshot, md::kMaxMarketDataInstruments> positions =
      {};
  std::size_t position_count = 0;
  model::AccountType type = model::AccountType::kCash;
  std::array<FuturesInitialPosition, md::kMaxMarketDataInstruments>
      futures_positions = {};
  std::size_t futures_position_count = 0;
  friend bool operator==(const TradingAccountSnapshot&,
                         const TradingAccountSnapshot&) noexcept = default;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ACCOUNT_H_
