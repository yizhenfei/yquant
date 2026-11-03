#include "yquant/trading/trading_service.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "yquant/trading/funds_terms.h"
#include "yquant/trading/internal/funds_calculation.h"
#include "yquant/trading/order.h"

namespace yquant::trading {
namespace {

// Uses the complete instrument ID and direction without allocating a key.
std::uint64_t TermsKey(std::uint32_t instrument, Side side) {
  return (static_cast<std::uint64_t>(instrument) << 1) |
         static_cast<std::uint64_t>(side == Side::kSell);
}

// Distinguishes missing rates from explicitly configured zero costs.
bool HasRequiredRates(const FundsTerms& terms, const OrderInfo& order) {
  return (!terms.hedge || *terms.hedge == order.hedge) &&
         terms.fees[static_cast<std::size_t>(order.offset)].has_value() &&
         (order.offset != Offset::kOpen || terms.margin_rate.has_value());
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

}  // namespace

std::optional<internal::OrderCost> internal::CalculateOrderCost(
    const OrderInfo& order, const FundsTerms& terms, double price,
    std::int32_t quantity) {
  if (quantity <= 0 || !std::isfinite(price) || price <= 0 ||
      !HasRequiredRates(terms, order)) return std::nullopt;
  const auto& fee = *terms.fees[static_cast<std::size_t>(order.offset)];
  const double notional = price * terms.contract_multiplier * quantity;
  if (!std::isfinite(notional) || notional <= 0) return std::nullopt;
  const double margin_rate =
      order.offset == Offset::kOpen ? *terms.margin_rate : 0;
  const double proportional_margin = notional * margin_rate;
  const double fixed_margin =
      order.offset == Offset::kOpen ? terms.margin_per_quantity * quantity : 0;
  const double margin = proportional_margin + fixed_margin;
  const double proportional_fee = fee.per_notional * notional;
  const double charge = fee.per_quantity * quantity + proportional_fee;
  if ((fixed_margin > 0 && proportional_margin > 0 &&
       (margin == fixed_margin || margin == proportional_margin)) ||
      !ProductRepresented(margin_rate, proportional_margin) ||
      !ProductRepresented(fee.per_notional, proportional_fee) ||
      !std::isfinite(margin + charge) ||
      (charge > 0 && margin + charge == margin) ||
      (margin > 0 && margin + charge == charge)) return std::nullopt;
  return OrderCost{margin, charge};
}

// Accepts supplied finite, nonnegative rates; absent values stay unknown.
bool TradingService::ValidTerms(const FundsTerms& terms) {
  const auto valid = [](double value) {
    return std::isfinite(value) && value >= 0;
  };
  return terms.currency.valid() && std::isfinite(terms.contract_multiplier) &&
         terms.contract_multiplier > 0 && valid(terms.margin_per_quantity) &&
         (!terms.margin_rate || valid(*terms.margin_rate)) &&
         std::all_of(terms.fees.begin(), terms.fees.end(),
                     [&](const auto& fee) {
                       return !fee || (valid(fee->per_quantity) &&
                                       valid(fee->per_notional));
                     });
}

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
    internal::OrderCost& cost) const {
  if (!account.funds_available || account.unpriced_orders != 0) {
    return FailureReason::kFundsUnavailable;
  }
  const auto* configured = FindFundsTerms(account, order);
  if (configured == nullptr) {
    return FailureReason::kFundsParametersUnavailable;
  }
  const auto& terms = *configured;
  if (!HasRequiredRates(terms, order)) {
    return FailureReason::kFundsParametersUnavailable;
  }
  if (terms.currency != account.funds.funds.currency) {
    return FailureReason::kCurrencyMismatch;
  }
  const auto required = internal::CalculateOrderCost(
      order, terms, order.price, order.quantity);
  if (!required) {
    return FailureReason::kInvalidFundsRequirement;
  }
  if (const auto failure = ReservationError(
          account.reserved_funds, required->total(),
          account.funds.funds.available)) {
    return failure;
  }
  cost = *required;
  return std::nullopt;
}

const FundsTerms* TradingService::FindFundsTerms(
    const AccountContext& account, const OrderInfo& order) const {
  const auto found = account.funds_terms.find(
      TermsKey(order.instrument_id, order.side));
  return found == account.funds_terms.end() ? nullptr : &found->second;
}

void TradingService::UpdateReservation(AccountContext& account,
                                       OrderRecord& order) {
  const auto filled = std::max(order.reported_filled_quantity,
                                order.detailed_filled_quantity);
  const bool pending = filled > order.booked_quantity;
  if (pending != order.funds_pending) {
    if (pending) {
      ++account.unpriced_orders;
    } else {
      --account.unpriced_orders;
    }
    order.funds_pending = pending;
  }
  auto held = order.order.quantity - order.booked_quantity;
  if (order.state == OrderState::kRejected ||
      order.state == OrderState::kSendFailed) {
    held = 0;
  } else if (order.state == OrderState::kCanceled) {
    held = filled - order.booked_quantity;
  }
  const double ratio = static_cast<double>(held) /
      (order.order.quantity - order.baseline_booked);
  const double required = order.initial_reservation * ratio;
  const double margin = order.initial_margin * ratio;
  auto count = account.reservation_count;
  if (order.reserved_funds == 0 && required > 0) ++count;
  if (order.reserved_funds > 0 && required == 0) --count;
  const auto total = count == 0 ? 0 : account.reserved_funds +
      (static_cast<long double>(required) - order.reserved_funds);
  const auto total_margin = count == 0 ? 0 : account.reserved_margin +
      (static_cast<long double>(margin) - order.reserved_margin);
  if (!std::isfinite(total) || !std::isfinite(total_margin) ||
      total < 0 || total_margin < 0 ||
      total > std::numeric_limits<double>::max() ||
      total_margin > std::numeric_limits<double>::max() ||
      (held > 0 && order.initial_reservation > 0 && required == 0) ||
      (required > order.reserved_funds && total <= account.reserved_funds)) {
    account.funds_available = false;
    Diagnose("reservation_unrepresentable", account.id, order.order.order_id);
    return;
  }
  account.reservation_count = count;
  account.reserved_funds = total;
  account.reserved_margin = total_margin;
  order.reserved_funds = required;
  order.reserved_margin = margin;
}

}  // namespace yquant::trading
