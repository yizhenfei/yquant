#ifndef YQUANT_TRADING_TRADING_CLIENT_H_
#define YQUANT_TRADING_TRADING_CLIENT_H_

#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Provides an interface for submitting trading requests and querying account
// state without exposing the service's deployment location. Use on the caller's
// business thread. Request and query contracts are defined by TradingService.
class TradingClient {
 public:
  virtual ~TradingClient() = default;
  // Binds the only callback receiver. Returns false if already bound or closed.
  // Bind before submitting requests; an unbound client may only query state.
  virtual bool BindListener(TradingListener& listener) = 0;
  // Reports whether Close can finish now without interrupting delivery.
  virtual bool CanClose() const = 0;
  // Detaches all callbacks, including queued notifications, without canceling
  // orders. Returns false if cleanup would interrupt delivery. Repeated Close succeeds.
  virtual bool Close() = 0;
  // Queries active orders and the caller's highest sequence; see TradingService.
  virtual bool QueryWorkingOrders(StrategyOrderScope scope, std::vector<OrderSnapshot>& orders,
                          std::uint64_t& last_sequence) const = 0;
  // Routes the queried orders to this client; see TradingService for validation.
  virtual bool AdoptWorkingOrders(StrategyOrderScope scope, std::span<const OrderSnapshot> orders) = 0;
  // kDelivered means the service received the request, including requests that
  // fail validation. Outcomes arrive after return through listener callbacks.
  // kIdExhausted or kUnavailable means no delivery and no callback.
  // Requests are unavailable until a listener is bound and after Close.
  virtual RequestResult SubmitOrder(const SubmitOrderRequest& request) = 0;
  // Requires a new cancel_request_id for each attempt on an order.
  // Delivery results follow SubmitOrder; counter results use OnCancelReport.
  virtual RequestResult CancelOrder(const CancelOrderRequest& request) = 0;
  // Returns the current snapshot for a matching order, account and strategy.
  // Returns false and leaves snapshot unchanged if no matching state is ready.
  virtual bool QueryOrder(const QueryOrderRequest& query,
                          OrderSnapshot& snapshot) const = 0;
  // Copies the account's latest available complete funds and returns true.
  // Returns false if the account is unknown or its funds are unavailable.
  // Leaves snapshot unchanged on false.
  virtual bool QueryFunds(const QueryFundsRequest& query,
                          FundsSnapshot& snapshot) const = 0;
  // Returns the account positions view; see PositionsSnapshot for view validity.
  // Returns false without changing snapshot if the account data is unavailable.
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
