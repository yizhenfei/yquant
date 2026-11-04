#ifndef YQUANT_TRADING_TRADING_CLIENT_FACTORY_H_
#define YQUANT_TRADING_TRADING_CLIENT_FACTORY_H_

#include <cstdint>
#include <memory>

#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {

// Creates trading clients by service ID so callers do not need to locate
// service instances or choose a concrete client class.
// Currently supports services in this process.
class TradingClientFactory {
 public:
  // registry must outlive this factory.
  explicit TradingClientFactory(const TradingServiceRegistry& registry)
      : registry_(registry) {}
  TradingClientFactory(const TradingClientFactory&) = delete;
  TradingClientFactory& operator=(const TradingClientFactory&) = delete;

  // Returns a local client for service_id, or nullptr for an unregistered ID.
  std::unique_ptr<TradingClient> Create(std::uint64_t service_id) const;

 private:
  const TradingServiceRegistry& registry_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_CLIENT_FACTORY_H_
