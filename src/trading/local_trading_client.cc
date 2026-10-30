#include "yquant/trading/local_trading_client.h"

#include <cstdint>
#include <limits>

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

}  // namespace yquant::trading
