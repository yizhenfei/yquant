#include "yquant/trading/local_trading_client.h"

#include <cstdint>
#include <cstdlib>
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
    const SubmitOrderRequest& request) {
  if (!listener_ || closed_) return RequestResult::kUnavailable;
  if (instance_id_ == 0 ||
      last_call_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return RequestResult::kIdExhausted;
  }
  registered_ = true;
  return service_.SubmitOrder(request, *this,
                              {instance_id_, ++last_call_id_});
}

RequestResult LocalTradingClient::CancelOrder(
    const CancelOrderRequest& request) {
  if (!listener_ || closed_) return RequestResult::kUnavailable;
  if (instance_id_ == 0 ||
      last_call_id_ == std::numeric_limits<std::uint64_t>::max()) {
    return RequestResult::kIdExhausted;
  }
  registered_ = true;
  return service_.CancelOrder(request, *this,
                              {instance_id_, ++last_call_id_});
}

LocalTradingClient::~LocalTradingClient() {
  if (!Close()) std::abort();
}

bool LocalTradingClient::BindListener(TradingListener& listener) {
  if (closed_ || listener_) return false;
  listener_ = &listener;
  return true;
}

bool LocalTradingClient::CanClose() const {
  return closed_ || !registered_ || service_.CanDetachListeners();
}

bool LocalTradingClient::Close() {
  if (closed_) return true;
  if (registered_ && !service_.DetachListener(*this)) return false;
  closed_ = true;
  listener_ = nullptr;
  return true;
}

void LocalTradingClient::OnRequestFailure(const RequestFailure& report) {
  if (listener_) listener_->OnRequestFailure(report);
}
void LocalTradingClient::OnOrderReport(const OrderReport& report) {
  if (listener_) listener_->OnOrderReport(report);
}
void LocalTradingClient::OnCancelReport(const CancelReport& report) {
  if (listener_) listener_->OnCancelReport(report);
}
void LocalTradingClient::OnTradeReport(const TradeReport& report) {
  if (listener_) listener_->OnTradeReport(report);
}

bool LocalTradingClient::QueryWorkingOrders(StrategyOrderScope scope,
    std::vector<OrderSnapshot>& orders, std::uint64_t& last_sequence) const {
  return !closed_ && service_.QueryWorkingOrders(scope, orders, last_sequence);
}

bool LocalTradingClient::AdoptWorkingOrders(StrategyOrderScope scope,
    std::span<const OrderSnapshot> orders) {
  if (!listener_ || closed_ || !service_.AdoptWorkingOrders(scope, orders, *this)) return false;
  registered_ = true;
  return true;
}

bool LocalTradingClient::QueryOrder(const QueryOrderRequest& query,
                                   OrderSnapshot& snapshot) const {
  return !closed_ && service_.QueryOrder(query, snapshot);
}

bool LocalTradingClient::QueryFunds(const QueryFundsRequest& query,
                                   FundsSnapshot& snapshot) const {
  return !closed_ && service_.QueryFunds(query, snapshot);
}

bool LocalTradingClient::QueryPositions(const QueryPositionsRequest& query,
                                       PositionsSnapshot& snapshot) const {
  return !closed_ && service_.QueryPositions(query, snapshot);
}

bool LocalTradingClient::QueryPosition(const QueryPositionRequest& query,
                                      PositionInfo& position) const {
  return !closed_ && service_.QueryPosition(query, position);
}

}  // namespace yquant::trading
