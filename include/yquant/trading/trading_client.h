#ifndef YQUANT_TRADING_TRADING_CLIENT_H_
#define YQUANT_TRADING_TRADING_CLIENT_H_

#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Provides trading requests and cached queries on the caller's business thread.
// Callers use the same interface across deployment locations.
// Keep listeners alive for the target service's runtime. Inputs are borrowed
// until return; reports are borrowed until their callback returns.
class TradingClient {
 public:
  virtual ~TradingClient() = default;
  // Removes all references to listener, including pending notifications.
  // Call between service operations. Returns false during a service callback
  // or request; success permits destroying the listener. Orders remain tracked.
  virtual bool DetachListener(TradingListener& listener) = 0;
  // Reads one strategy's active orders and caller sequence floor after recovery.
  virtual bool QueryWorkingOrders(StrategyOrderScope scope, std::vector<OrderSnapshot>& orders,
                          std::uint64_t& last_sequence) const = 0;
  // Claims the previously read set together. Failure leaves all bindings intact.
  virtual bool AdoptWorkingOrders(StrategyOrderScope scope, std::span<const OrderSnapshot> orders,
                          TradingListener& listener) = 0;
  // kDelivered means the service received the request, including requests that
  // fail validation. Outcomes arrive after return through listener callbacks.
  // kIdExhausted means no delivery and no callback for this invocation.
  virtual RequestResult SubmitOrder(const SubmitOrderRequest& request,
                                    TradingListener& listener) = 0;
  // Uses a new cancel_request_id for each attempt. Delivery and listener
  // lifetime follow SubmitOrder; counter results arrive through OnCancelReport.
  virtual RequestResult CancelOrder(const CancelOrderRequest& request,
                                    TradingListener& listener) = 0;
  // Returns the current snapshot for a matching order, account and strategy.
  // Returns false and leaves snapshot unchanged if no matching state is ready.
  virtual bool QueryOrder(const QueryOrderRequest& query,
                          OrderSnapshot& snapshot) const = 0;
  // Copies the account's latest available complete funds and returns true.
  // Returns false if the account is unknown or its funds are unavailable.
  // Leaves snapshot unchanged on false.
  // Both account snapshots and the initialization boundary must be complete.
  // Unpriced fills or accounting errors make the account data unavailable.
  // Late initialization results cannot replace the running account state.
  virtual bool QueryFunds(const QueryFundsRequest& query,
                          FundsSnapshot& snapshot) const = 0;
  // Borrows complete account positions on true; the row range may be empty.
  // On false the account is unknown/unavailable and snapshot stays unchanged.
  // Copy rows before the next position update or service/cache destruction.
  virtual bool QueryPositions(const QueryPositionsRequest& query,
                              PositionsSnapshot& snapshot) const = 0;
  // Copies one holding and returns true. An absent valid key produces zeros.
  // Returns false without changing position if the account is unknown,
  // positions are unavailable, or the key is invalid.
  virtual bool QueryPosition(const QueryPositionRequest& query,
                             PositionInfo& position) const = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_CLIENT_H_
