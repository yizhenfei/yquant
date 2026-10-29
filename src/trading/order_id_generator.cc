#include "yquant/trading/order_id_generator.h"

#include <limits>
#include <optional>

#include "yquant/trading/order.h"

namespace yquant::trading {

std::optional<OrderId> OrderIdGenerator::Next() {
  if (sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }
  return OrderId{caller_service_id_, ++sequence_};
}

}  // namespace yquant::trading
