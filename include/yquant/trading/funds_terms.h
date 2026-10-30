#ifndef YQUANT_TRADING_FUNDS_TERMS_H_
#define YQUANT_TRADING_FUNDS_TERMS_H_

#include <array>
#include <optional>

#include "yquant/model/currency.h"

namespace yquant::trading {

// Estimates fees in currency units from order quantity and notional value.
struct FeeRate {
  double per_quantity = 0;
  double per_notional = 0;
};

// Owns the parameters for one account, instrument and order direction.
// fees is indexed by Offset; a missing rate is unknown, while FeeRate{} is a
// known zero fee. margin_rate is needed only for opening orders.
struct FundsTerms {
  model::Currency currency;
  double contract_multiplier = 0;
  std::optional<double> margin_rate;
  std::array<std::optional<FeeRate>, 4> fees;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_FUNDS_TERMS_H_
