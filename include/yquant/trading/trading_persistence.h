#ifndef YQUANT_TRADING_TRADING_PERSISTENCE_H_
#define YQUANT_TRADING_TRADING_PERSISTENCE_H_

#include <stddef.h>
#include <stdint.h>

#include <span>

#include "yquant/base/status.h"
#include "yquant/model/trading_event.h"

namespace yquant::trading {

// Each fact describes one validated ledger transition. Sending is represented
// by a fact committed before the external call, so replay never sends again.
enum class TradingMutationKind : uint8_t {
  kNewOrder = 1,
  kCancel,
  kSendNew,
  kSendCancel,
  kScheduleLocalCancel,
  kLocalCancel,
  kReport,
};

// Owned inputs to a deterministic transition. Only request for kNewOrder and
// event for kReport are used; IDs identify the expected affected request.
struct TradingMutation {
  TradingMutationKind kind = TradingMutationKind::kNewOrder;
  uint64_t revision = 0;
  uint64_t now = 0;
  model::ClientOrderId client_order_id;
  model::CancelRequestId cancel_id;
  model::OrderRequest request;
  model::TradingEvent event;
};

// Called serially after validation and before any business-state change or
// external send. Must not reenter TradingEngine. Failure preserves the ledger;
// success means the caller can commit without any further fallible operation.
// The borrowed fact is valid only during this call. No durability wait is
// required: a mapped-journal implementation may acknowledge a page-cache write.
class TradingMutationSink {
 public:
  virtual ~TradingMutationSink() noexcept = default;
  [[nodiscard]] virtual base::Status BeforeCommit(
      const TradingMutation& mutation) noexcept = 0;
};

inline constexpr size_t kMaxEncodedTradingMutation = 1024;
inline constexpr size_t kMaxTradingCheckpointBytes = 4 * 1024 * 1024;

// Explicit versioned little-endian encoding; no native layouts or padding.
// Encode failure preserves size; decode failure preserves out. Neither
// operation allocates. The enclosing journal/checkpoint supplies its checksum.
[[nodiscard]] base::Status EncodeTradingMutation(
    const TradingMutation& mutation, std::span<std::byte> bytes,
    size_t& size) noexcept;
[[nodiscard]] base::Status DecodeTradingMutation(
    std::span<const std::byte> bytes, TradingMutation& out) noexcept;

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_PERSISTENCE_H_
