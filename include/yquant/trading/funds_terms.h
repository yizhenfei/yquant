#ifndef YQUANT_TRADING_FUNDS_TERMS_H_
#define YQUANT_TRADING_FUNDS_TERMS_H_

#include <array>
#include <optional>

#include "yquant/model/currency.h"
#include "yquant/trading/order.h"

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
  // Applies only to Offset::kClose. Explicit today/yesterday offsets take
  // priority. Set this to match the counter's generic-close allocation rule.
  bool close_today_first = false;
  // Fixed opening margin per lot, charged in addition to margin_rate.
  double margin_per_quantity = 0;
  // Account defaults apply to every hedge category unless a rate query
  // identifies a specific category.
  std::optional<HedgeFlag> hedge = std::nullopt;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_FUNDS_TERMS_H_
