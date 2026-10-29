#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {

bool TradingServiceRegistry::Register(std::uint64_t service_id,
                                     TradingService& service) {
  return services_.emplace(service_id, &service).second;
}

TradingService* TradingServiceRegistry::Find(std::uint64_t service_id) const {
  const auto found = services_.find(service_id);
  return found == services_.end() ? nullptr : found->second;
}

}  // namespace yquant::trading
