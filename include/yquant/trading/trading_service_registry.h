#ifndef YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_
#define YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_

#include <cstdint>
#include <unordered_map>

#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Maps service IDs to TradingService instances in this process.
// Does not own the services. Each registered service must outlive this registry.
class TradingServiceRegistry {
 public:
  TradingServiceRegistry() = default;
  TradingServiceRegistry(const TradingServiceRegistry&) = default;
  TradingServiceRegistry& operator=(const TradingServiceRegistry&) = default;

  // Registers service under service_id and returns true. An existing ID returns
  // false without replacing its service, even when registering the same object.
  bool Register(std::uint64_t service_id, TradingService& service);
  // Returns the service registered under service_id, or nullptr if none exists.
  TradingService* Find(std::uint64_t service_id) const;

 private:
  std::unordered_map<std::uint64_t, TradingService*> services_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_
