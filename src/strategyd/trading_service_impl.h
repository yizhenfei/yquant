#ifndef YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_
#define YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <optional>

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/trading_client_config.h"
#include "yquant/trading/trading_service.h"

namespace yquant::strategyd {

// Implements the strategy trading API over one authenticated tradingd session.
// Submissions enter a local queue; authoritative account reservations and risk
// acceptance belong to the gateway. The runner opens request entry only during
// business callbacks, then closes it before processing transport or events.
//
// Serialize all access on the owning strategy thread. Init allocates bounded
// session storage; query methods return copies and preserve outputs on false.
// A failed stream ends the session without automatic resend or reconnect.
// Cached views remain available after Stop for shutdown diagnostics; Reset
// releases them. Shutdown must drain through the runner before Stop closes TCP.
class TradingServiceImpl final : public trading::TradingService {
 public:
  TradingServiceImpl(const TradingServiceImpl&) = delete;
  TradingServiceImpl& operator=(const TradingServiceImpl&) = delete;

  // Borrows clock and instruments until destruction. The configured scope must
  // stay unchanged while the client is initialized.
  TradingServiceImpl(const base::MonotonicClock& clock,
                     const md::InstrumentScope& instruments) noexcept;
  ~TradingServiceImpl() noexcept override;
  // Allocates session storage and completes a timeout-bounded handshake before
  // exposing the initial account. Copies config and dictionary values needed
  // by the connection; neither input is retained by reference. Requires Reset
  // before reuse after Stop. A failed first Init releases partial storage and
  // transport, retaining diagnostic text. Reinitializing an existing session
  // returns kInvalidState and preserves that session.
  [[nodiscard]] base::Status Init(
      const trading::TradingClientConfig& config,
      const md::MarketDataConfig& dictionary) noexcept override;
  // Closes transport and request entry without erasing cached facts or waiting
  // for acknowledgement. Repeated calls succeed and preserve diagnostics.
  [[nodiscard]] base::Status Stop() noexcept override {
    accept_requests_ = false;
    stopping_ = true;
    connection_.Close();
    return {};
  }
  // Closes transport, frees all cached state, clears detail text, and permits
  // initialization again. Existing order IDs no longer identify stored orders.
  void Reset() noexcept override;
  void SetAcceptingRequests(bool accepting) noexcept override {
    accept_requests_ = accepting && !stopping_;
  }
  void BeginStopping() noexcept override;
  bool accepting_orders() const noexcept override;
  bool supports_generic_close() const noexcept override;
  // Queues a validated request and overwrites out with its new local ID.
  // Borrows request only for this call. Failure preserves out and cached facts,
  // updating diagnostic text. Local acceptance reserves no account funds and
  // does not imply gateway acceptance.
  [[nodiscard]] base::ErrorCode SubmitOrder(
      const model::OrderRequest& request,
      model::StrategyOrderId& out) noexcept override;
  // Queues a cancellation for a working order without another pending cancel.
  // Success marks the snapshot cancel-pending; rejection preserves the order
  // and queue while updating diagnostics. Requires an open request window.
  [[nodiscard]] base::ErrorCode CancelOrder(
      model::StrategyOrderId id) noexcept override;
  [[nodiscard]] bool TryGetOrder(
      model::StrategyOrderId id,
      model::OrderSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetCash(
      model::CashSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetPosition(
      model::InstrumentId id,
      model::PositionSnapshot& out) const noexcept override;
  // Copies the gateway futures account; cash or uninitialized sessions return
  // false. Uses the cached authoritative view, without recalculating margin.
  [[nodiscard]] bool TryGetFuturesAccount(
      model::FuturesAccountSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetFuturesPosition(
      model::InstrumentId id,
      model::FuturesPositionSnapshot& out) const noexcept override;
  // Services transport, heartbeat, and up to limit queued requests with entry
  // closed. Uses host-monotonic now. During stopping, unsent new orders become
  // local cancellations and a Stop message follows the request queue. Errors
  // retain earlier progress; transport/heartbeat failures disable the session.
  [[nodiscard]] base::Status ProcessRequests(
      uint64_t now, size_t limit = 64) noexcept override;
  // Returns an absolute host-monotonic deadline: zero for queued work or an
  // unsent Stop, the next poll time otherwise, or nullopt without storage.
  std::optional<uint64_t> next_deadline() const noexcept override;
  bool has_ready_event(uint64_t now) const noexcept override;
  // Requires the gateway's Drained acknowledgement, empty local/transport
  // queues, and terminal orders with no pending cancels. Without session
  // storage there is no work to drain, so returns true.
  bool is_drained() const noexcept override;
  size_t order_count() const noexcept override;
  // Returns non-null client-owned diagnostics. Later operations may replace
  // detail or stage; Stop preserves both and Reset clears only detail.
  const char* error_detail() const noexcept override { return error_detail_; }
  const char* error_stage() const noexcept override { return error_stage_; }
  // Alternates ready local cancellations with gateway messages, committing
  // each accepted message's complete view before returning. A notification
  // sets event=true. Consumed messages overwrite out even without notification;
  // empty transport instead preserves out, sets event=false, and defers
  // polling. Failure preserves both outputs. Peer/transport errors disable the
  // session; invalid lifecycle or request-window calls update only diagnostics.
  [[nodiscard]] base::Status Poll(uint64_t now, trading::TradingDispatch& out,
                                  bool& event) noexcept override;

  // Queues configured futures quotes for the gateway's simulator; other quotes
  // and uninitialized/cash sessions are ignored. Borrows quote only during the
  // call. Queue failure records detail and is returned for host error shutdown.
  [[nodiscard]] base::Status OnQuote(
      const model::QuoteTick& quote) noexcept override;

 private:
  // Owns bounded session queues, order/fill history, and the gateway account.
  struct Storage;
  // Returns whether the session has queued before-send cancellations. Poll
  // combines this with transport readiness to choose the next event fairly.
  bool has_local_updates() const noexcept;
  // Commits one cancellation of an order never attempted on transport and
  // overwrites out. Called by Poll with request entry closed. Failure preserves
  // cached facts, the local queue, and out.
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(
      trading::TradingDispatch& out) noexcept;
  // Checks a nonzero revision and, unless initial, stable account type and
  // nonregressing revision. Equal revisions require identical account contents.
  // Failure records detail without replacing the cached account.
  [[nodiscard]] base::ErrorCode ValidateAccount(
      const trading::AccountView& account, bool initial) noexcept;
  // Validates one report against local identity and history, then commits its
  // order, optional fill, and account before overwriting out. now stamps local
  // receive times. Exact duplicates update the account but produce no dispatch.
  // Failure preserves those views and out, updating only diagnostics.
  [[nodiscard]] base::ErrorCode ApplyUpdate(
      const trading::Message& message, uint64_t now,
      trading::TradingDispatch& out) noexcept;
  // Copies non-null C-string detail into bounded storage and returns code.
  // Does not change the operation stage or mark the session failed.
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  // Marks an allocated session failed, copies non-null C-string detail, and
  // returns status unchanged. Cached facts remain available for diagnostics.
  base::Status FailSession(base::Status status, const char* detail) noexcept;

  const base::MonotonicClock& clock_;
  const md::InstrumentScope& instruments_;
  std::unique_ptr<Storage> storage_;
  trading::Connection connection_;
  // Alternates local and wire events when both are ready.
  bool prefer_local_ = true;
  const char* error_stage_ = "trading_connect";  // Borrowed static stage name.
  bool accept_requests_ = false;
  bool stopping_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_
