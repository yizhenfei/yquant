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
  return service == nullptr ? nullptr
                            : std::make_unique<LocalTradingClient>(*service);
}

}  // namespace yquant::trading
