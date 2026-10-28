#include "yquant/market/market_service_registry.h"

#include <cstdint>

#include "yquant/market/market_service.h"

namespace yquant::market {

bool MarketServiceRegistry::Register(std::uint64_t service_id,
                                    MarketService& service) {
  return services_.try_emplace(service_id, &service).second;
}

MarketService* MarketServiceRegistry::Find(std::uint64_t service_id) const {
  const auto entry = services_.find(service_id);
  return entry == services_.end() ? nullptr : entry->second;
}

}  // namespace yquant::market
