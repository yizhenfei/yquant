#ifndef YQUANT_SRC_TRADING_SIMNOW_CANCELLATIONS_H_
#define YQUANT_SRC_TRADING_SIMNOW_CANCELLATIONS_H_

#include <ThostFtdcTraderApi.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "yquant/trading/trading_adapter.h"

namespace yquant::trading::internal {

// Copies only the target identity echoed by a native action error.
// A direct response may carry just its request ID.
struct ActionError {
  int request_id = 0, action_ref = 0, front = 0, session = 0;
  std::string ref, exchange, system_id, symbol, memo;
};

// Tracks all in-flight cancellations of each order on the business thread.
// Native request IDs disambiguate individual rejections. A terminal cancel
// achieves the requested outcome for every still-pending attempt on that order.
class SimNowCancellations {
 public:
  void Start(int front, int session);
  // Assigns both native action identifiers and saves the attempt before send.
  void Register(AdapterCancelOrderRequest attempt, int id,
                CThostFtdcInputOrderActionField& action);
  // A nonzero send return removes the attempt from the pending success set.
  void NotSent(int id);
  bool HasRequest(int id) const { return attempts_.contains(id); }
  // id is supplied by a direct SDK response; zero denotes a private error
  // stream where the action reference and issuer session must both match.
  void Reject(int id, const ActionError& error, ITradingAdapterCallback& sink);
  // Returns true if the service received at least one cancellation report.
  bool Complete(OrderId order, int filled, ITradingAdapterCallback& sink);

 private:
  void RemovePending(OrderId order, int id);
  // Associates a service attempt with the echoed target used to check errors.
  struct Attempt {
    AdapterCancelOrderRequest request;
    ActionError target;
  };
  std::string memo_;
  std::unordered_map<int, Attempt> attempts_;
  std::unordered_map<OrderId, std::vector<int>, OrderIdHash> pending_;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_SRC_TRADING_SIMNOW_CANCELLATIONS_H_
