#ifndef YQUANT_TRADING_TRADING_SERVICE_H_
#define YQUANT_TRADING_TRADING_SERVICE_H_

#include <cstddef>
#include <cstdint>
#include <optional>

#include "yquant/md/market_data_config.h"
#include "yquant/strategy/trading.h"
#include "yquant/trading/trading_config.h"
#include "yquant/trading/trading_dispatch.h"

namespace yquant::trading {

// Abstract host-side trading service shared by local and remote sessions.
// Implementations own execution state; hosts serialize request entry, transport
// work and account/order commits.
class TradingService : public strategy::Trading {
 public:
  // Initializes the selected transport and complete initial account before
  // strategy creation. Inputs are borrowed only during this call. Failure is
  // safe to Stop; error_stage/detail remain valid until the next operation.
  [[nodiscard]] virtual base::Status Init(
      const TradingConfig& config,
      const md::MarketDataConfig& dictionary) noexcept = 0;
  // Releases transport resources after callbacks and unconfirmed-order reports.
  // Safe after partial Init; does not erase ledger facts. Reset then frees
  // them.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  virtual void Reset() noexcept = 0;
  // The host opens a serial request window only after initialization. Strategy
  // hosts restrict it to running business callbacks; gateways apply their own
  // client checks. Close the window before transport or event processing.
  // BeginStopping permanently closes it until Reset.
  virtual void SetAcceptingRequests(bool accepting) noexcept = 0;
  virtual void BeginStopping() noexcept = 0;
  // Performs bounded transport work outside callbacks.
  [[nodiscard]] virtual base::Status ProcessRequests(
      std::uint64_t now, std::size_t limit = 64) noexcept = 0;
  [[nodiscard]] virtual base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept = 0;
  virtual std::optional<std::uint64_t> next_deadline() const noexcept = 0;
  // True when Poll can consume an event now; request/retry work alone is not
  // an event. next_deadline includes both kinds of work.
  virtual bool has_ready_event(std::uint64_t now) const noexcept = 0;
  virtual bool is_drained() const noexcept = 0;
  virtual std::size_t order_count() const noexcept = 0;
  virtual const char* error_detail() const noexcept = 0;
  virtual const char* error_stage() const noexcept = 0;
  // Commits at most one local or transport event, rotating fairly when both
  // are ready. Success sets event; idle preserves out and advances the polled
  // transport deadline beyond now (or removes it). Other local work may remain
  // ready. Failure preserves both outputs.
  [[nodiscard]] virtual base::Status Poll(std::uint64_t now,
                                          TradingDispatch& out,
                                          bool& event) noexcept = 0;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_SERVICE_H_
