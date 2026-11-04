#ifndef YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_
#define YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_

#include <cstdint>

#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Routes local trading reports to one receiver and removes all of its callback
// associations on Close. Orders continue to be tracked by the service.
// Destroy between service calls while the service is still available.
class LocalTradingClient final : public TradingClient, private TradingListener {
 public:
  explicit LocalTradingClient(TradingService& service);
  ~LocalTradingClient() override;
  LocalTradingClient(const LocalTradingClient&) = delete;
  LocalTradingClient& operator=(const LocalTradingClient&) = delete;

  // TradingClient implementation.
  bool BindListener(TradingListener& listener) override;
  bool CanClose() const override;
  bool Close() override;
  bool QueryWorkingOrders(StrategyOrderScope scope, std::vector<OrderSnapshot>& orders,
                          std::uint64_t& last_sequence) const override;
  bool AdoptWorkingOrders(StrategyOrderScope scope, std::span<const OrderSnapshot> orders) override;
  RequestResult SubmitOrder(const SubmitOrderRequest& request) override;
  RequestResult CancelOrder(const CancelOrderRequest& request) override;
  bool QueryOrder(const QueryOrderRequest& query,
                  OrderSnapshot& snapshot) const override;
  bool QueryFunds(const QueryFundsRequest& query,
                  FundsSnapshot& snapshot) const override;
  bool QueryPositions(const QueryPositionsRequest& query,
                      PositionsSnapshot& snapshot) const override;
  bool QueryPosition(const QueryPositionRequest& query,
                     PositionInfo& position) const override;

 private:
  // TradingListener implementation.
  void OnRequestFailure(const RequestFailure& report) override;
  void OnOrderReport(const OrderReport& report) override;
  void OnCancelReport(const CancelReport& report) override;
  void OnTradeReport(const TradeReport& report) override;

  TradingListener* listener_ = nullptr;
  bool closed_ = false;
  bool registered_ = false;
  TradingService& service_;
  std::uint64_t instance_id_;
  std::uint64_t last_call_id_ = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_LOCAL_TRADING_CLIENT_H_
