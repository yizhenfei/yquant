#ifndef YQUANT_TRADING_ORDER_ID_GENERATOR_H_
#define YQUANT_TRADING_ORDER_ID_GENERATOR_H_

#include <cstdint>
#include <optional>

#include "yquant/trading/order.h"

namespace yquant::trading {

// Generates order IDs for one caller service on its business thread.
// Share it across that service's strategies, accounts and target services.
class OrderIdGenerator {
 public:
  // Starts after last_sequence, which defaults to no previously allocated IDs.
  explicit OrderIdGenerator(std::uint64_t caller_service_id,
                            std::uint64_t last_sequence = 0)
      : caller_service_id_(caller_service_id), sequence_(last_sequence) {}
  OrderIdGenerator(const OrderIdGenerator&) = delete;
  OrderIdGenerator& operator=(const OrderIdGenerator&) = delete;

  std::uint64_t caller_service_id() const { return caller_service_id_; }
  // Ensures the next ID is above sequence; never decreases the current sequence.
  void AdvancePast(std::uint64_t sequence) {
    if (sequence > sequence_) sequence_ = sequence;
  }
  // Returns a new ID, or nullopt when the sequence is exhausted. Never wraps.
  std::optional<OrderId> Next();

 private:
  std::uint64_t caller_service_id_;
  std::uint64_t sequence_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_ORDER_ID_GENERATOR_H_
