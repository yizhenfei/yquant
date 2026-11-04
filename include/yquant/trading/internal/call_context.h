#ifndef YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_
#define YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_

#include <cstdint>

namespace yquant::trading::internal {

// Identifies one client call so the service can match its response even when
// multiple calls use the same order ID. Used internally; business listeners
// receive order IDs instead of this context.
struct CallContext {
  // Identifies the client instance that made the call.
  std::uint64_t client_instance_id = 0;
  // Identifies this call within that client instance.
  std::uint64_t call_id = 0;
  bool operator==(const CallContext&) const = default;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_
