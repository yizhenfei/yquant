#ifndef YQUANT_TRADING_TRADING_CLIENT_FACTORY_H_
#define YQUANT_TRADING_TRADING_CLIENT_FACTORY_H_

#include <cstdint>
#include <memory>

#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {

// Creates clients using a borrowed typed registry supplied by program assembly.
// Keep the registry alive for this factory's lifetime. Returned clients are
// owned by callers and may outlive the factory and registry. Keep each service
// alive until its clients and report delivery have finished.
class TradingClientFactory {
 public:
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
