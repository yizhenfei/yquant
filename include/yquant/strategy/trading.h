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
  // Atomically registers PendingNew and a queued request, then sets out. kOk
  // means local acceptance, not gateway risk approval or broker acceptance.
  // Local equity execution reserves cash/shares; local futures execution
  // reserves opening margin plus fees or closing inventory plus fees. A valid
  // futures close remains allowed with negative available funds. With remote
  // tradingd, final reservation belongs to the shared gateway and insufficient
  // resources cause an asynchronous rejection. Failure changes neither state,
  // queue nor out. Returns kInvalidState outside running business callbacks,
  // kUnsupported
  // without an adapter, kInvalidArgument for bad parameters or amount overflow,
  // or kResourceExhausted for exhausted order/request storage and IDs (also
  // insufficient cash/shares with the in-process adapter).
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
  // Copies equity-account cash, including confirmed buy reservations. With a
  // remote gateway this is shared account state and may lag local requests or
  // other clients; callbacks observe the account paired with their event.
  // Disabled/futures services return false and preserve out.
  [[nodiscard]] virtual bool TryGetCash(
      model::CashSnapshot& out) const noexcept = 0;
  // Copies the equity position, with the same freshness rules as cash. Known
  // omitted equity instruments return zero. Disabled/futures service or an
  // unknown instrument returns false without modifying out.
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
