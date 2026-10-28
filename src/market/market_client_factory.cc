#include "yquant/market/market_client_factory.h"

#include <cstdint>
#include <memory>

#include "yquant/market/local_market_client.h"
#include "yquant/market/market_client.h"
#include "yquant/market/market_service.h"
#include "yquant/market/market_service_registry.h"

namespace yquant::market {

std::unique_ptr<MarketClient> MarketClientFactory::Create(
    std::uint64_t service_id) const {
  MarketService* service = local_services_.Find(service_id);
  if (service == nullptr) {
    return nullptr;
  }
  return std::make_unique<LocalMarketClient>(*service);
}

}  // namespace yquant::market
