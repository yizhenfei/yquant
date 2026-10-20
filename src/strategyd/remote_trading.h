#ifndef YQUANT_STRATEGYD_REMOTE_TRADING_H_
#define YQUANT_STRATEGYD_REMOTE_TRADING_H_

#include <memory>

#include "strategyd/trading_config.h"
#include "strategyd/trading_runtime.h"
#include "tradingd/connection.h"

namespace yquant::strategyd {

// Main-thread TCP trading client. Orders are locally queued, while account
// reservations and final risk acceptance belong to tradingd. A failed stream
// terminates this session; uncertain commands are never resent automatically.
class RemoteTrading final : public TradingRuntime {
 public:
  RemoteTrading(const MonotonicClock& clock,
                const MarketDataImpl& market_data) noexcept;
  ~RemoteTrading() noexcept override;
  // Allocates fixed storage and completes the bounded authenticated handshake
  // before exposing an account. Failure leaves the client disabled and owns
  // a diagnostic. Dictionary and config are borrowed only during this call.
  [[nodiscard]] base::Status Init(const TradingConfig& config,
                                  const MarketDataConfig& dictionary) noexcept;
  void Reset() noexcept override;
  void SetRunning(bool running) noexcept override { running_ = running; }
  void BeginCallback(CallbackPhase phase) noexcept override { phase_ = phase; }
  void EndCallback() noexcept override { phase_ = CallbackPhase::kNone; }
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
      TradingAdapter& unused, std::uint64_t now,
      std::size_t limit = 64) noexcept override;
  [[nodiscard]] base::Status ScheduleStopping(
      std::uint64_t now, std::size_t max_scan = 64) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool has_local_updates() const noexcept override;
  bool is_drained() const noexcept override;
  std::size_t order_count() const noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }
  [[nodiscard]] base::ErrorCode ApplyEvent(
      const TradingEvent& event, TradingDispatch& out) noexcept override;
  [[nodiscard]] base::ErrorCode ApplyLocalUpdate(
      TradingDispatch& out) noexcept override;
  // Commits at most one incoming message and its complete account before
  // callbacks. Idle sets event=false and advances polling to a future deadline.
  // Failure preserves out and event and makes the session unusable.
  [[nodiscard]] base::Status Poll(std::uint64_t now, TradingDispatch& out,
                                  bool& event) noexcept;

 private:
  struct Storage;
  // Validates a complete account, including unknown local instruments, without
  // mutating the cached view. Equal revisions require identical content.
  [[nodiscard]] base::ErrorCode ValidateAccount(
      const tradingd::AccountView& account, bool initial) noexcept;
  // Validates identity, state and increments before committing order/account.
  [[nodiscard]] base::ErrorCode ApplyUpdate(const tradingd::Message& message,
                                            std::uint64_t now,
                                            TradingDispatch& out) noexcept;
  base::ErrorCode Fail(base::ErrorCode code, const char* detail) noexcept;
  // Marks a session unusable after uncertain transport or invalid peer data.
  base::Status FailSession(base::Status status, const char* detail) noexcept;

  const MonotonicClock& clock_;
  const MarketDataImpl& market_data_;
  std::unique_ptr<Storage> storage_;
  tradingd::Connection connection_;
  bool running_ = false;
  bool stopping_ = false;
  CallbackPhase phase_ = CallbackPhase::kNone;
  char error_detail_[256] = {};
};

}  // namespace yquant::strategyd
#endif  // YQUANT_STRATEGYD_REMOTE_TRADING_H_
