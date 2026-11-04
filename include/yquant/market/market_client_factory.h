#ifndef YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_
#define YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_

#include <cstdint>
#include <memory>

#include "yquant/market/market_client.h"
#include "yquant/market/market_service_registry.h"

namespace yquant::market {

// Creates market clients by service ID so callers do not need to locate
// service instances or choose a concrete client class.
// Currently supports services in this process.
class MarketClientFactory {
 public:
  // local_services must outlive this factory.
  explicit MarketClientFactory(const MarketServiceRegistry& local_services)
      : local_services_(local_services) {}
  MarketClientFactory(const MarketClientFactory&) = delete;
  MarketClientFactory& operator=(const MarketClientFactory&) = delete;

  // Creates a client for the service registered under service_id.
  // Returns nullptr if the ID is not registered.
  std::unique_ptr<MarketClient> Create(std::uint64_t service_id) const;

 private:
  const MarketServiceRegistry& local_services_;
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MARKET_CLIENT_FACTORY_H_
