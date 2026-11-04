#include "yquant/trading/trading_client_factory.h"

#include <cstdint>
#include <memory>

#include "yquant/trading/local_trading_client.h"
#include "yquant/trading/trading_client.h"
#include "yquant/trading/trading_service_registry.h"

namespace yquant::trading {

std::unique_ptr<TradingClient> TradingClientFactory::Create(
    std::uint64_t service_id) const {
  auto* service = registry_.Find(service_id);
  if (!service) return nullptr;
  return std::make_unique<LocalTradingClient>(*service);
}

std::unique_ptr<TradingClient> TradingClientFactory::Create(
    std::uint64_t service_id, TradingListener& listener) const {
  auto client = Create(service_id);
  if (client) client->BindListener(listener);
  return client;
}

}  // namespace yquant::trading
