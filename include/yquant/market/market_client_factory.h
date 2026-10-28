#ifndef YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_
#define YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_

#include <cstdint>
#include <memory>

#include "yquant/market/market_client.h"
#include "yquant/market/market_service_registry.h"

namespace yquant::market {

// Creates same-process market clients using a borrowed local service registry.
// Keep the registry alive until this factory is destroyed. Returned clients
// are owned by callers. Keep each target service alive until its clients are
// destroyed.
class MarketClientFactory {
 public:
  explicit MarketClientFactory(const MarketServiceRegistry& local_services)
      : local_services_(local_services) {}
  MarketClientFactory(const MarketClientFactory&) = delete;
  MarketClientFactory& operator=(const MarketClientFactory&) = delete;

  // Creates a client connected to service_id, or returns nullptr for an
  // unregistered ID. The client can outlive this factory and its registry.
  std::unique_ptr<MarketClient> Create(std::uint64_t service_id) const;

 private:
  const MarketServiceRegistry& local_services_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_
