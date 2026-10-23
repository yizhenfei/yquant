#ifndef YQUANT_TRADING_TRADING_ADAPTER_H_
#define YQUANT_TRADING_TRADING_ADAPTER_H_

#include <stdint.h>

#include <optional>

#include "yquant/base/status.h"
#include "yquant/model/order.h"
#include "yquant/model/quote_tick.h"
#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_config.h"

namespace yquant::trading {

enum class TradingPollState { kEvent, kAccount, kIdle, kError };

// Only kEvent changes Poll's output; kAccount signals an updated cached broker
// account readable through ReadBrokerAccountSnapshot. Other outcomes preserve
// the caller's event. status is successful except for kError.
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
  [[nodiscard]] virtual base::Status Start(uint64_t now) noexcept = 0;
  // Announces irreversible gateway shutdown. This only changes adapter-local
  // control state; all transport and final broker-account work stays in Poll.
  // Called once after Prepare, including when startup was only partial.
  virtual void BeginStopping() noexcept {}
  // Copies the initial owned account after Start succeeds and before any order
  // is sent. Performs no allocation; failure preserves out. The host validates
  // instrument membership before publishing this snapshot to the strategy.
  [[nodiscard]] virtual base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept = 0;
  // Broker-managed adapters expose a complete coherent snapshot after Start
  // and each kEvent/kAccount Poll. Failure preserves out; no SDK views escape.
  // Simulators keep the unsupported default and use ReadAccountSnapshot.
  [[nodiscard]] virtual base::Status ReadBrokerAccountSnapshot(
      BrokerAccountSnapshot&) noexcept {
    return {base::ErrorCode::kUnsupported, {}};
  }
  // Observes an accepted quote before the strategy callback. Simulation may
  // schedule matching facts, delivered later through Poll; never calls back
  // into the strategy. Live adapters can retain this no-op implementation.
  [[nodiscard]] virtual base::Status OnQuote(const model::QuoteTick&,
                                             uint64_t) noexcept {
    return {};
  }
  // Success means adapter-local acceptance, not broker acknowledgement. Inputs
  // are borrowed during the call only. The host treats any failed send as an
  // uncertain attempt and never automatically resends the same request.
  [[nodiscard]] virtual base::Status SendOrder(
      model::ClientOrderId id, const model::OrderRequest& request,
      uint64_t now) noexcept = 0;
  // Must also settle requests registered before an order finished filling;
  // such orders produce an associated rejection rather than being forgotten.
  [[nodiscard]] virtual base::Status SendCancel(
      model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
      model::CancelRequestId cancel_request_id, uint64_t now) noexcept = 0;
  // Nonblocking, allocation-free and bounded work; emits at most one event.
  // Idle/error preserve out. now shares the host's monotonic clock domain.
  [[nodiscard]] virtual TradingPollResult Poll(
      uint64_t now, model::TradingEvent& out) noexcept = 0;
  // Earliest pending event; empty if none. Idle must not retain a due deadline.
  virtual std::optional<uint64_t> next_deadline() const noexcept = 0;
  // True only with no active orders, outstanding requests or pending events.
  virtual bool is_drained() const noexcept = 0;
  // Idempotent synchronous resource release, safe after partial preparation.
  // It does not confirm broker cancellation or alter host ledger facts.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  // Host-owned text, valid until the next operation; Stop preserves failures.
  virtual const char* error_detail() const noexcept = 0;
};

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_TRADING_ADAPTER_H_
