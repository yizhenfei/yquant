#ifndef YQUANT_STRATEGYD_TRADING_ADAPTER_H_
#define YQUANT_STRATEGYD_TRADING_ADAPTER_H_

#include <cstdint>
#include <optional>

#include "strategyd/trading_config.h"
#include "yquant/base/status.h"
#include "yquant/model/order.h"
#include "yquant/model/trading_event.h"

namespace yquant::strategyd {

// Self-contained normalized input to the host ledger. Sequence starts at one
// for each order; non-fill events leave fill empty. No field borrows SDK data.
struct TradingEvent {
  model::ClientOrderId client_order_id;
  model::BrokerOrderId broker_order_id;
  std::uint64_t report_sequence = 0;
  model::OrderUpdateKind kind = model::OrderUpdateKind::kAccepted;
  std::int64_t cumulative_filled_qty = 0;
  std::uint64_t receive_time_ns = 0;
  model::Fill fill;
  model::CancelRequestId cancel_request_id;
  model::OrderReason reason;
};

enum class TradingPollState { kEvent, kIdle, kError };

// Only kEvent changes Poll's output; status is successful except for kError.
struct TradingPollResult {
  TradingPollState state = TradingPollState::kIdle;
  base::Status status;
};

// Main-thread adapter boundary. All operations are serialized and never invoke
// strategies. Accepted sends copy their inputs; Poll returns owned events.
class TradingAdapter {
 public:
  virtual ~TradingAdapter() noexcept = default;

  // Initializes bounded storage without sending requests. Failure remains safe
  // to Stop; a successful Prepare is followed by at most one Start.
  [[nodiscard]] virtual base::Status Prepare(
      const TradingConfig& config) noexcept = 0;
  [[nodiscard]] virtual base::Status Start(std::uint64_t now) noexcept = 0;
  // Copies the initial owned account after Start succeeds and before any order
  // is sent. Performs no allocation; failure preserves out. The host validates
  // instrument membership before publishing this snapshot to the strategy.
  [[nodiscard]] virtual base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept = 0;
  // Success means adapter-local acceptance, not broker acknowledgement. Inputs
  // are borrowed during the call only. The host treats any failed send as an
  // uncertain attempt and never automatically resends the same request.
  [[nodiscard]] virtual base::Status SendOrder(
      model::ClientOrderId id, const model::OrderRequest& request,
      std::uint64_t now) noexcept = 0;
  // Must also settle requests registered before an order finished filling;
  // such orders produce an associated rejection rather than being forgotten.
  [[nodiscard]] virtual base::Status SendCancel(
      model::ClientOrderId id, const model::BrokerOrderId& broker_order_id,
      model::CancelRequestId cancel_request_id, std::uint64_t now) noexcept = 0;
  // Nonblocking, allocation-free and bounded work; emits at most one event.
  // Idle/error preserve out. now shares the host's monotonic clock domain.
  [[nodiscard]] virtual TradingPollResult Poll(std::uint64_t now,
                                               TradingEvent& out) noexcept = 0;
  // Earliest pending event; empty if none. Idle must not retain a due deadline.
  virtual std::optional<std::uint64_t> next_deadline() const noexcept = 0;
  // True only with no active orders, outstanding requests or pending events.
  virtual bool is_drained() const noexcept = 0;
  // Idempotent synchronous resource release, safe after partial preparation.
  // It does not confirm broker cancellation or alter host ledger facts.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  // Host-owned text, valid until the next operation; Stop preserves failures.
  virtual const char* error_detail() const noexcept = 0;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_TRADING_ADAPTER_H_
