#ifndef YQUANT_TRADING_POSITION_H_
#define YQUANT_TRADING_POSITION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>

#include "yquant/trading/order.h"

namespace yquant::trading {

enum class PositionSide { kLong, kShort };

// Identifies one account holding by instrument, direction and hedge category.
struct PositionKey {
  std::uint32_t instrument_id = 0;
  PositionSide side = PositionSide::kLong;
  HedgeFlag hedge = HedgeFlag::kSpeculation;
  // Selects the holding opened or closed by this order.
  static PositionKey ForOrder(const OrderInfo& order) {
    const auto long_side =
        order.offset == Offset::kOpen ? Side::kBuy : Side::kSell;
    return {order.instrument_id,
            order.side == long_side ? PositionSide::kLong : PositionSide::kShort,
            order.hedge};
  }
  bool operator==(const PositionKey&) const = default;

  // Hashes the instrument, position side and hedge category for map lookup.
  struct Hash {
    std::size_t operator()(const PositionKey& key) const {
      return (std::hash<std::uint32_t>{}(key.instrument_id) << 3) ^
             (static_cast<std::size_t>(key.side) << 2) ^
             static_cast<std::size_t>(key.hedge);
    }
  };
};

// Stores the cost basis and occupied margin for today's or yesterday's position.
// Both amounts are expressed in the account currency.
struct PositionCost {
  // Total cost basis: price basis * contract multiplier * position quantity.
  // Uses the weighted-average opening price for today's position,
  // or the previous trading day's settlement price for yesterday's position.
  double cost = 0;

  // Margin occupied by this position.
  double margin = 0;

  bool operator==(const PositionCost&) const = default;
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
  // Nonempty buckets require costs; an empty bucket may omit them.
  std::optional<PositionCost> today_cost = std::nullopt;
  std::optional<PositionCost> yesterday_cost = std::nullopt;
  std::int32_t today_available() const { return today_quantity - today_frozen; }
  std::int32_t yesterday_available() const {
    return yesterday_quantity - yesterday_frozen;
  }
  // Returns the quantity available for a generic close across both buckets.
  std::int64_t available_quantity() const {
    return static_cast<std::int64_t>(today_available()) + yesterday_available();
  }
  bool operator==(const PositionInfo&) const = default;
};

// Borrows a complete account view; an empty positions range is a valid result.
// Copy rows needed beyond the next position update or destruction of the
// service or cache that owns the rows.
struct PositionsSnapshot {
  std::span<const PositionInfo> positions;
  std::uint64_t refresh_id = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_POSITION_H_
