#ifndef YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_
#define YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_

#include <cstdint>

namespace yquant::trading::internal {

// Correlates a client invocation and response when order IDs are duplicated.
// Services retain this context internally; business listeners use order IDs.
struct CallContext {
  std::uint64_t client_instance_id = 0;
  std::uint64_t call_id = 0;
  bool operator==(const CallContext&) const = default;
};

}  // namespace yquant::trading::internal

#endif  // YQUANT_TRADING_INTERNAL_CALL_CONTEXT_H_
