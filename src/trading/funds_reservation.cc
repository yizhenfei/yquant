#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "yquant/trading/funds_terms.h"
#include "yquant/trading/order.h"

namespace yquant::trading {
namespace {

// Uses the complete instrument ID and direction without allocating a key.
std::uint64_t TermsKey(std::uint32_t instrument, Side side) {
  return (static_cast<std::uint64_t>(instrument) << 1) |
         static_cast<std::uint64_t>(side == Side::kSell);
}

// Accepts supplied finite, nonnegative rates; absent values stay unknown.
bool ValidTerms(const FundsTerms& terms) {
  const auto valid = [](double value) {
    return std::isfinite(value) && value >= 0;
  };
  return terms.currency.valid() && std::isfinite(terms.contract_multiplier) &&
         terms.contract_multiplier > 0 &&
         (!terms.margin_rate || valid(*terms.margin_rate)) &&
         std::all_of(terms.fees.begin(), terms.fees.end(),
                     [&](const auto& fee) {
                       return !fee || (valid(fee->per_quantity) &&
                                       valid(fee->per_notional));
                     });
}

// Distinguishes missing rates from explicitly configured zero costs.
bool HasRequiredRates(const FundsTerms& terms, Offset offset) {
  return terms.fees[static_cast<std::size_t>(offset)].has_value() &&
         (offset != Offset::kOpen || terms.margin_rate.has_value());
}

// Positive proportional costs must not disappear through arithmetic underflow.
bool ProductRepresented(double rate, double cost) {
  return rate == 0 || cost > 0;
}

// Checks the account total without mutating the reservation ledger.
std::optional<FailureReason> ReservationError(long double previous,
                                            double required, double available) {
  const long double total = previous + required;
  if (!std::isfinite(total) || (required > 0 && total <= previous)) {
    return FailureReason::kInvalidFundsRequirement;
  }
  if (required > 0 && total > available) {
    return FailureReason::kInsufficientFunds;
  }
  return std::nullopt;
}

// Calculates from validated order parameters and known applicable rates.
// Returns no amount if arithmetic cannot represent the required funds.
std::optional<double> RequiredFunds(const OrderInfo& order,
                                    const FundsTerms& terms) {
  const auto& fee = *terms.fees[static_cast<std::size_t>(order.offset)];
  const double notional =
      order.price * terms.contract_multiplier * order.quantity;
  if (!std::isfinite(notional) || notional <= 0) {
    return std::nullopt;
  }
  const double margin_rate =
      order.offset == Offset::kOpen ? *terms.margin_rate : 0;
  const double margin = notional * margin_rate;
  const double proportional_fee = fee.per_notional * notional;
  if (!ProductRepresented(margin_rate, margin) ||
      !ProductRepresented(fee.per_notional, proportional_fee)) {
    return std::nullopt;
  }
  const double amount = margin + fee.per_quantity * order.quantity +
                        proportional_fee;
  return std::isfinite(amount) ? std::optional<double>(amount) : std::nullopt;
}

}  // namespace

bool TradingService::ConfigureFundsTerms(std::uint64_t account_id,
                                         std::uint32_t instrument_id, Side side,
                                         const FundsTerms& terms) {
  const auto account = account_index_.find(account_id);
  if (account == account_index_.end() || running_ || request_depth_ ||
      callback_depth_ || !orders_.empty()) {
    return false;
  }
  if (instrument_id == 0 || (side != Side::kBuy && side != Side::kSell) ||
      !ValidTerms(terms)) {
    return false;
  }
  account->second->funds_terms.insert_or_assign(TermsKey(instrument_id, side),
                                                terms);
  return true;
}

std::optional<FailureReason> TradingService::CheckFunds(
    const AccountContext& account, const OrderInfo& order,
    double& amount) const {
  if (!account.funds_available) {
    return FailureReason::kFundsUnavailable;
  }
  const auto found = account.funds_terms.find(TermsKey(order.instrument_id,
                                                       order.side));
  if (found == account.funds_terms.end()) {
    return FailureReason::kFundsParametersUnavailable;
  }
  const auto& terms = found->second;
  if (!HasRequiredRates(terms, order.offset)) {
    return FailureReason::kFundsParametersUnavailable;
  }
  if (terms.currency != account.funds.funds.currency) {
    return FailureReason::kCurrencyMismatch;
  }
  const auto required = RequiredFunds(order, terms);
  if (!required) {
    return FailureReason::kInvalidFundsRequirement;
  }
  if (const auto failure = ReservationError(account.reserved_funds, *required,
                                            account.funds.funds.available)) {
    return failure;
  }
  amount = *required;
  return std::nullopt;
}

}  // namespace yquant::trading
