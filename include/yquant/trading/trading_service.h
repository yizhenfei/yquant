#ifndef YQUANT_TRADING_TRADING_SERVICE_H_
#define YQUANT_TRADING_TRADING_SERVICE_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>

#include "yquant/market/market_data_config.h"
#include "yquant/strategy/trading.h"
#include "yquant/trading/trading_client_config.h"
#include "yquant/trading/trading_dispatch.h"

namespace yquant::trading {

// Strategy-host trading service for gateway requests and cached query views.
// Production connects to tradingd; unit tests inject a scripted service double.
// Hosts serialize request entry, transport work and cached-view commits.
class TradingService : public strategy::Trading {
 public:
  // Connects to the gateway and obtains its complete account view before
  // strategy creation. Inputs are borrowed only during this call. Failure is
  // safe to Stop; error_stage/detail remain valid until the next operation.
  [[nodiscard]] virtual base::Status Init(
      const TradingClientConfig& config,
      const market::MarketDataConfig& dictionary) noexcept = 0;
  // Releases transport resources after callbacks and unconfirmed-order reports.
  // Safe after partial Init; does not erase ledger facts. Reset then frees
  // them.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  virtual void Reset() noexcept = 0;
  // The host opens a serial request window only after initialization. Strategy
  // hosts restrict it to running business callbacks. Close the window before
  // transport or event processing.
  // BeginStopping permanently closes it until Reset.
  virtual void SetAcceptingRequests(bool accepting) noexcept = 0;
  virtual void BeginStopping() noexcept = 0;
  // Copies an accepted source quote for a simulation gateway. Live execution
  // ignores quotes. Called outside callbacks; failures preserve caller input.
  [[nodiscard]] virtual base::Status OnQuote(
      const model::QuoteTick& quote) noexcept = 0;
  // Performs bounded transport work outside callbacks.
  [[nodiscard]] virtual base::Status ProcessRequests(
      uint64_t now, size_t limit = 64) noexcept = 0;
  virtual std::optional<uint64_t> next_deadline() const noexcept = 0;
  // True when Poll can consume an event now; request/retry work alone is not
  // an event. next_deadline includes both kinds of work.
  virtual bool has_ready_event(uint64_t now) const noexcept = 0;
  virtual bool is_drained() const noexcept = 0;
  virtual size_t order_count() const noexcept = 0;
  virtual const char* error_detail() const noexcept = 0;
  virtual const char* error_stage() const noexcept = 0;
  // Commits at most one local or transport event, rotating fairly when both
  // are ready. Success sets event; idle preserves out and advances the polled
  // transport deadline beyond now (or removes it). Other local work may remain
  // ready. Failure preserves both outputs.
  [[nodiscard]] virtual base::Status Poll(uint64_t now, TradingDispatch& out,
                                          bool& event) noexcept = 0;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_TRADING_SERVICE_H_
