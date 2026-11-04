#include "yquant/trading/local_trading_client.h"

#include <cstdint>
#include <limits>

#include "yquant/trading/funds.h"
#include "yquant/trading/position.h"
#include "yquant/trading/trading_listener.h"
#include "yquant/trading/trading_service.h"

namespace yquant::trading {
namespace {

// Allocates process-local client identities on the single business thread.
// Returns zero on exhaustion so no invocation can reuse an earlier identity.
std::uint64_t NextClientId() {
  static std::uint64_t last_id = 0;
  return last_id == std::numeric_limits<std::uint64_t>::max() ? 0 : ++last_id;
}

}  // namespace

LocalTradingClient::LocalTradingClient(TradingService& service)
    : service_(service), instance_id_(NextClientId()) {}

RequestResult LocalTradingClient::SubmitOrder(
    const SubmitOrderRequest& request, TradingListener& listener) {
  if (instance_id_ == 0 ||
      last_call_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return RequestResult::kIdExhausted;
  }
  return service_.SubmitOrder(request, listener,
                              {instance_id_, ++last_call_id_});
}

RequestResult LocalTradingClient::CancelOrder(
    const CancelOrderRequest& request, TradingListener& listener) {
  if (instance_id_ == 0 ||
      last_call_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return RequestResult::kIdExhausted;
  }
  return service_.CancelOrder(request, listener,
                              {instance_id_, ++last_call_id_});
}

bool LocalTradingClient::DetachListener(TradingListener& listener) {
  return service_.DetachListener(listener);
}

bool LocalTradingClient::QueryWorkingOrders(StrategyOrderScope scope,
    std::vector<OrderSnapshot>& orders, std::uint64_t& last_sequence) const {
  return service_.QueryWorkingOrders(scope, orders, last_sequence);
}

bool LocalTradingClient::AdoptWorkingOrders(StrategyOrderScope scope,
    std::span<const OrderSnapshot> orders, TradingListener& listener) {
  return service_.AdoptWorkingOrders(scope, orders, listener);
}

bool LocalTradingClient::QueryOrder(const QueryOrderRequest& query,
                                   OrderSnapshot& snapshot) const {
  return service_.QueryOrder(query, snapshot);
}

bool LocalTradingClient::QueryFunds(const QueryFundsRequest& query,
                                   FundsSnapshot& snapshot) const {
  return service_.QueryFunds(query, snapshot);
}

bool LocalTradingClient::QueryPositions(const QueryPositionsRequest& query,
                                       PositionsSnapshot& snapshot) const {
  return service_.QueryPositions(query, snapshot);
}

bool LocalTradingClient::QueryPosition(const QueryPositionRequest& query,
                                      PositionInfo& position) const {
  return service_.QueryPosition(query, position);
}

}  // namespace yquant::trading
