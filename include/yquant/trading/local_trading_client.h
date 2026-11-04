#ifndef YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_
#define YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_

#include <cstdint>

#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Calls a borrowed local service and assigns distinct invocation identifiers.
// The service must outlive this client. Destroying the client does not cancel
// orders or detach listeners; keep listeners alive for the service's runtime.
class LocalTradingClient final : public TradingClient {
 public:
  explicit LocalTradingClient(TradingService& service);
  LocalTradingClient(const LocalTradingClient&) = delete;
  LocalTradingClient& operator=(const LocalTradingClient&) = delete;

  // TradingClient implementation.
  bool DetachListener(TradingListener& listener) override;
  // Reads one strategy's active orders and caller sequence floor after recovery.
  bool QueryWorkingOrders(StrategyOrderScope scope, std::vector<OrderSnapshot>& orders,
                          std::uint64_t& last_sequence) const override;
  // Claims the previously read set together. Failure leaves all bindings intact.
  bool AdoptWorkingOrders(StrategyOrderScope scope, std::span<const OrderSnapshot> orders,
                          TradingListener& listener) override;
  RequestResult SubmitOrder(const SubmitOrderRequest& request,
                            TradingListener& listener) override;
  RequestResult CancelOrder(const CancelOrderRequest& request,
                            TradingListener& listener) override;
  bool QueryOrder(const QueryOrderRequest& query,
                  OrderSnapshot& snapshot) const override;
  bool QueryFunds(const QueryFundsRequest& query,
                  FundsSnapshot& snapshot) const override;
  bool QueryPositions(const QueryPositionsRequest& query,
                      PositionsSnapshot& snapshot) const override;
  bool QueryPosition(const QueryPositionRequest& query,
                     PositionInfo& position) const override;

 private:
  TradingService& service_;
  std::uint64_t instance_id_;
  std::uint64_t last_call_id_ = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_
