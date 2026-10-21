#ifndef YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_
#define YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_

#include <memory>

#include "yquant/base/monotonic_clock.h"
#include "yquant/md/instrument_scope.h"
#include "yquant/trading/connection.h"
#include "yquant/trading/trading_client_config.h"
#include "yquant/trading/trading_service.h"

namespace yquant::strategyd {

// Main-thread TCP trading client. Orders are locally queued, while account
// reservations and final risk acceptance belong to tradingd. A failed stream
// terminates this session; uncertain commands are never resent automatically.
class TradingServiceImpl final : public trading::TradingService {
 public:
  // Borrows clock and instrument scope for the entire client lifetime.
  TradingServiceImpl(const base::MonotonicClock& clock,
                     const md::InstrumentScope& instruments) noexcept;
  ~TradingServiceImpl() noexcept override;
  // Allocates fixed storage and completes the bounded authenticated handshake
  // before exposing an account. Failure leaves the client disabled and owns
  // a diagnostic. Dictionary and config are borrowed only during this call.
  [[nodiscard]] base::Status Init(
      const trading::TradingClientConfig& config,
      const md::MarketDataConfig& dictionary) noexcept override;
  [[nodiscard]] base::Status Stop() noexcept override {
    accept_requests_ = false;
    stopping_ = true;
    connection_.Close();
    return {};
  }
  void Reset() noexcept override;
  void SetAcceptingRequests(bool accepting) noexcept override {
    accept_requests_ = accepting && !stopping_;
  }
  void BeginStopping() noexcept override;
  bool accepting_orders() const noexcept override;
  // Success only commits a local request; tradingd may subsequently reject it.
  // Local acceptance does not reserve the cached authoritative account view.
  [[nodiscard]] base::ErrorCode SubmitOrder(
      const model::OrderRequest& request,
      model::ClientOrderId& out) noexcept override;
  [[nodiscard]] base::ErrorCode CancelOrder(
      model::ClientOrderId id) noexcept override;
  [[nodiscard]] bool TryGetOrder(
      model::ClientOrderId id,
      model::OrderSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetCash(
      model::CashSnapshot& out) const noexcept override;
  [[nodiscard]] bool TryGetPosition(
      model::InstrumentId id,
      model::PositionSnapshot& out) const noexcept override;
  [[nodiscard]] base::Status ProcessRequests(
      std::uint64_t now, std::size_t limit = 64) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool has_local_updates() const noexcept;
  bool has_ready_event(std::uint64_t now) const noexcept override;
  bool is_drained() const noexcept override;
  std::size_t order_count() const noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }
  const char* error_stage() const noexcept override { return error_stage_; }
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(
      trading::TradingDispatch& out) noexcept;
  // Commits at most one incoming message and its complete account before
  // callbacks. Idle sets event=false and advances polling to a future deadline.
  // Failure preserves out and event and makes the session unusable.
  [[nodiscard]] base::Status Poll(std::uint64_t now,
                                  trading::TradingDispatch& out,
                                  bool& event) noexcept override;

 private:
  struct Storage;
  // Validates a complete account, including unknown local instruments, without
  // mutating the cached view. Equal revisions require identical content.
  [[nodiscard]] base::ErrorCode ValidateAccount(
      const trading::AccountView& account, bool initial) noexcept;
  // Validates identity, state and increments before committing order/account.
  [[nodiscard]] base::ErrorCode ApplyUpdate(
      const trading::Message& message, std::uint64_t now,
      trading::TradingDispatch& out) noexcept;
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  // Marks a session unusable after uncertain transport or invalid peer data.
  base::Status FailSession(base::Status status, const char* detail) noexcept;

  const base::MonotonicClock& clock_;
  const md::InstrumentScope& instruments_;
  std::unique_ptr<Storage> storage_;
  trading::Connection connection_;
  bool prefer_local_ = true;
  const char* error_stage_ = "trading_connect";
  bool accept_requests_ = false;
  bool stopping_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_TRADING_SERVICE_IMPL_H_
