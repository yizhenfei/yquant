#ifndef YQUANT_TRADING_INTERNAL_FUNDS_CALCULATION_H_
#define YQUANT_TRADING_INTERNAL_FUNDS_CALCULATION_H_

#include <cstdint>
#include <optional>

#include "yquant/trading/funds_terms.h"
#include "yquant/trading/order.h"

namespace yquant::trading::internal {

// Separates occupied margin from fees for reservations and fill booking.
struct OrderCost {
  double margin = 0;
  double fee = 0;
  double total() const { return margin + fee; }
};

// Prices a quantity using a validated order and configured terms. Missing
// rates, invalid price/quantity or unrepresentable costs return nullopt.
std::optional<OrderCost> CalculateOrderCost(const OrderInfo& order,
                                           const FundsTerms& terms,
                                           double price, std::int32_t quantity);

}  // namespace yquant::trading::internal

#endif  // YQUANT_TRADING_INTERNAL_FUNDS_CALCULATION_H_
