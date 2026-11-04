#ifndef YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_
#define YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_

#include <cstdint>
#include <unordered_map>

#include "yquant/market/market_service.h"

namespace yquant::market {

// Maps service IDs to MarketService instances in this process.
// Does not own the services. Each registered service must outlive this registry.
class MarketServiceRegistry {
 public:
  MarketServiceRegistry() = default;
  MarketServiceRegistry(const MarketServiceRegistry&) = default;
  MarketServiceRegistry& operator=(const MarketServiceRegistry&) = default;

  // Associates service_id with service and returns true.
  // If service_id is already registered, returns false and keeps the
  // previously registered service, even if it is the same service.
  bool Register(std::uint64_t service_id, MarketService& service);
  // Returns a pointer to the service registered under service_id,
  // or nullptr if the ID is not registered.
  MarketService* Find(std::uint64_t service_id) const;

 private:
  std::unordered_map<std::uint64_t, MarketService*> services_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_SERVICE_REGISTRY_H_
