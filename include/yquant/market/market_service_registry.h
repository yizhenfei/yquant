#ifndef YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_
#define YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_

#include <cstdint>
#include <unordered_map>

#include "yquant/market/market_service.h"

namespace yquant::market {

// Maps local service IDs to borrowed MarketService instances.
// Keep registered services alive until this registry is destroyed.
class MarketServiceRegistry {
 public:
  MarketServiceRegistry() = default;
  MarketServiceRegistry(const MarketServiceRegistry&) = default;
  MarketServiceRegistry& operator=(const MarketServiceRegistry&) = default;

  // Registers service under service_id. Returns false if the ID is already
  // registered, preserving its original service binding.
  bool Register(std::uint64_t service_id, MarketService& service);
  // Returns the registered service, or nullptr if service_id is not registered.
  // The pointer does not transfer ownership and is valid until the service
  // is destroyed.
  MarketService* Find(std::uint64_t service_id) const;

 private:
  std::unordered_map<std::uint64_t, MarketService*> services_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_
