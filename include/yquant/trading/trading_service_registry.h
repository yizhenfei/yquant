#ifndef YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_
#define YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_

#include <cstdint>
#include <unordered_map>

#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Maps local service IDs to borrowed concrete instances during assembly.
// Keep registered services alive until the registry is destroyed.
class TradingServiceRegistry {
 public:
  TradingServiceRegistry() = default;
  TradingServiceRegistry(const TradingServiceRegistry&) = default;
  TradingServiceRegistry& operator=(const TradingServiceRegistry&) = default;

  // Returns false for an existing ID without replacing its service binding.
  bool Register(std::uint64_t service_id, TradingService& service);
  // Returns a borrowed service pointer, or nullptr if the ID is unregistered.
  TradingService* Find(std::uint64_t service_id) const;

 private:
  std::unordered_map<std::uint64_t, TradingService*> services_;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_SERVICE_REGISTRY_H_
