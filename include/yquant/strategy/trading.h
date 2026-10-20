#ifndef YQUANT_STRATEGY_TRADING_H_
#define YQUANT_STRATEGY_TRADING_H_

#include "yquant/base/status.h"
#include "yquant/model/account.h"
#include "yquant/model/order.h"

namespace yquant::strategy {

// Host-owned order/account service borrowed during the current strategy
// callback. Queries copy owned data. Requests never invoke callbacks
// synchronously.
class Trading {
 public:
  virtual ~Trading() noexcept = default;
  // True only during running business callbacks with a configured adapter;
  // does not promise valid parameters or available capacity.
  virtual bool accepting_orders() const noexcept = 0;
  // Atomically reserves buy cash or sellable shares, registers PendingNew and
  // a queued request, then sets out. kOk does
  // not mean broker acceptance. Failure changes neither state, queue nor out.
  // Returns kInvalidState outside running business callbacks, kUnsupported
  // without an adapter, kInvalidArgument for bad parameters or amount overflow,
  // or kResourceExhausted for insufficient available cash/shares or exhausted
  // order/request storage and IDs. Failure preserves reservations and balances.
  [[nodiscard]] virtual base::ErrorCode SubmitOrder(
      const model::OrderRequest& request,
      model::ClientOrderId& out) noexcept = 0;
  // Locally queues one cancel attempt on Working without an existing attempt.
  // The order can still fill. Failure preserves all state and queued requests.
  // Uses the same phase/adapter/capacity errors as SubmitOrder; an unknown ID
  // returns kInvalidArgument, other order states return kInvalidState.
  [[nodiscard]] virtual base::ErrorCode CancelOrder(
      model::ClientOrderId id) noexcept = 0;
  // Includes terminal orders until teardown. Unknown IDs preserve out.
  [[nodiscard]] virtual bool TryGetOrder(
      model::ClientOrderId id, model::OrderSnapshot& out) const noexcept = 0;
  // Copies configured cash, including pending buy reservations. Disabled
  // service returns false and preserves out.
  [[nodiscard]] virtual bool TryGetCash(
      model::CashSnapshot& out) const noexcept = 0;
  // Known instruments omitted from bootstrap return a zero position. Disabled
  // service or unknown instrument returns false without modifying out.
  [[nodiscard]] virtual bool TryGetPosition(
      model::InstrumentId id, model::PositionSnapshot& out) const noexcept = 0;
};
}  // namespace yquant::strategy
#endif  // YQUANT_STRATEGY_TRADING_H_
