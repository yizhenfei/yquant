#ifndef YQUANT_TRADING_POSITION_H_
#define YQUANT_TRADING_POSITION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>

namespace yquant::trading {

enum class PositionSide { kLong, kShort };
enum class HedgeFlag { kSpeculation, kArbitrage, kHedge };

// Identifies one account holding by instrument, direction and hedge category.
struct PositionKey {
  std::uint32_t instrument_id = 0;
  PositionSide side = PositionSide::kLong;
  HedgeFlag hedge = HedgeFlag::kSpeculation;
  bool operator==(const PositionKey&) const = default;
};

// Combines all identity fields; equality compares the complete key.
struct PositionKeyHash {
  std::size_t operator()(const PositionKey& key) const {
    return (std::hash<std::uint32_t>{}(key.instrument_id) << 3) ^
           (static_cast<std::size_t>(key.side) << 2) ^
           static_cast<std::size_t>(key.hedge);
  }
};

// Stores today's and yesterday's positions and their frozen quantities.
// today_frozen cannot exceed today_quantity; yesterday_frozen cannot exceed
// yesterday_quantity. All four quantities are nonnegative.
struct PositionInfo {
  PositionKey key;
  std::int32_t today_quantity = 0;
  std::int32_t yesterday_quantity = 0;
  std::int32_t today_frozen = 0;
  std::int32_t yesterday_frozen = 0;
  bool operator==(const PositionInfo&) const = default;
};

// Borrows a complete account view; an empty positions range is a valid result.
// Copy rows needed beyond the next successful refresh or destruction of the
// service or cache that owns the rows.
struct PositionsSnapshot {
  std::span<const PositionInfo> positions;
  std::uint64_t refresh_id = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_POSITION_H_
