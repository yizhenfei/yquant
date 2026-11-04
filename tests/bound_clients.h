#ifndef YQUANT_TESTS_BOUND_CLIENTS_H_
#define YQUANT_TESTS_BOUND_CLIENTS_H_

#include <memory>
#include <unordered_map>

#include "yquant/trading/trading_client_factory.h"

namespace yquant::test {

// Gives legacy counter scenarios a distinct, persistent client per receiver.
class BoundClients {
 public:
  explicit BoundClients(trading::TradingService& service) { registry_.Register(1, service); }
  trading::TradingClient& Get(trading::TradingListener& listener) {
    auto& client = clients_[&listener];
    if (!client) client = trading::TradingClientFactory(registry_).Create(1, listener);
    return *client;
  }

 private:
  trading::TradingServiceRegistry registry_;
  std::unordered_map<trading::TradingListener*, std::unique_ptr<trading::TradingClient>> clients_;
};

}  // namespace yquant::test

#endif  // YQUANT_TESTS_BOUND_CLIENTS_H_
