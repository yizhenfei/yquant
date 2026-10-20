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
  // Atomically registers PendingNew and a queued request, then sets out. kOk
  // means local acceptance, not gateway risk approval or broker acceptance.
  // In-process mock trading also reserves local cash/shares here. With a remote
  // tradingd, final reservation belongs to the shared gateway; insufficient
  // resources are reported asynchronously as an order rejection. Local failure
  // changes neither state, queue nor out.
  // Returns kInvalidState outside running business callbacks, kUnsupported
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
  // Copies the latest cash view, including confirmed buy reservations. With a
  // remote gateway this is shared account state and may lag local requests or
  // other clients; callbacks observe the account view paired with their event.
  // Disabled service returns false and preserves out.
  [[nodiscard]] virtual bool TryGetCash(
      model::CashSnapshot& out) const noexcept = 0;
  // Copies the latest account position, with the same freshness rules as cash.
  // Known omitted instruments return zero. Disabled service or unknown
  // instrument returns false without modifying out.
  [[nodiscard]] virtual bool TryGetPosition(
      model::InstrumentId id, model::PositionSnapshot& out) const noexcept = 0;
};
}  // namespace yquant::strategy
#endif  // YQUANT_STRATEGY_TRADING_H_
