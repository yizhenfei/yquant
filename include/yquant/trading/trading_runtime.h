#ifndef YQUANT_TRADING_TRADING_RUNTIME_H_
#define YQUANT_TRADING_TRADING_RUNTIME_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "yquant/model/trading_event.h"
#include "yquant/strategy/callback_phase.h"
#include "yquant/strategy/trading.h"

namespace yquant::trading {
class TradingAdapter;
struct TradingEvent;

// Fully committed values owned by the host until serial callback dispatch.
struct TradingDispatch {
  model::OrderUpdate update;
  model::Fill fill;
  bool notify_order = false;
  bool has_fill = false;
};

// Main-thread service lifecycle shared by local and remote execution. The
// runner never interleaves callbacks with request transport or ledger commits.
class TradingRuntime : public strategy::Trading {
 public:
  virtual void Reset() noexcept = 0;
  virtual void SetRunning(bool running) noexcept = 0;
  virtual void BeginCallback(strategy::CallbackPhase phase) noexcept = 0;
  virtual void EndCallback() noexcept = 0;
  virtual void BeginStopping() noexcept = 0;
  // Performs bounded transport work outside callbacks. The remote runtime
  // owns its connection and does not use the supplied local adapter.
  [[nodiscard]] virtual base::Status ProcessRequests(
      TradingAdapter& adapter, std::uint64_t now,
      std::size_t limit = 64) noexcept = 0;
  [[nodiscard]] virtual base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept = 0;
  virtual std::optional<std::uint64_t> next_deadline() const noexcept = 0;
  virtual bool has_local_updates() const noexcept = 0;
  virtual bool is_drained() const noexcept = 0;
  virtual std::size_t order_count() const noexcept = 0;
  virtual const char* error_detail() const noexcept = 0;
  // Successful values are fully committed before dispatch; failures preserve
  // output. ApplyEvent is used only with an injected/local trading adapter.
  [[nodiscard]] virtual base::ErrorCode ApplyEvent(
      const TradingEvent& event, TradingDispatch& out) noexcept = 0;
  [[nodiscard]] virtual base::ErrorCode ApplyLocalUpdate(
      TradingDispatch& out) noexcept = 0;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_RUNTIME_H_
