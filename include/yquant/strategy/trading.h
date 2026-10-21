#ifndef YQUANT_STRATEGY_TRADING_H_
#define YQUANT_STRATEGY_TRADING_H_

#include "yquant/base/status.h"
#include "yquant/model/account.h"
#include "yquant/model/futures.h"
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
  // Atomically reserves buy cash/sellable shares for equity, or opening margin
  // plus fees/closing inventory plus fees for futures, registers PendingNew
  // and a queued request, then sets out. kOk does
  // not mean broker acceptance. Failure changes neither state, queue nor out.
  // Futures open requires sufficient available funds; valid closing orders
  // remain allowed with negative available funds. Returns kInvalidState outside
  // running business callbacks, kUnsupported
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
  // Copies configured equity-account cash, including buy reservations.
  // Disabled/futures services return false and preserve out.
  [[nodiscard]] virtual bool TryGetCash(
      model::CashSnapshot& out) const noexcept = 0;
  // Cash-account positions omitted from bootstrap return a zero position.
  // Disabled/futures service or unknown instrument preserves out and returns
  // false.
  [[nodiscard]] virtual bool TryGetPosition(
      model::InstrumentId id, model::PositionSnapshot& out) const noexcept = 0;
  // Futures-only owned account/position copies. Cash accounts return false;
  // unknown instruments return false. Failure preserves out. A configured
  // futures instrument without inventory returns an empty position.
  [[nodiscard]] virtual bool TryGetFuturesAccount(
      model::FuturesAccountSnapshot& out) const noexcept = 0;
  [[nodiscard]] virtual bool TryGetFuturesPosition(
      model::InstrumentId id,
      model::FuturesPositionSnapshot& out) const noexcept = 0;
};
}  // namespace yquant::strategy
#endif  // YQUANT_STRATEGY_TRADING_H_
