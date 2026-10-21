#ifndef YQUANT_TRADING_DISABLED_TRADING_RUNTIME_H_
#define YQUANT_TRADING_DISABLED_TRADING_RUNTIME_H_

#include "yquant/trading/trading_service.h"

namespace yquant::trading {

// Explicitly disabled trading service. Queries preserve outputs; requests in
// running business callbacks return kUnsupported, other phases kInvalidState.
class DisabledTradingRuntime final : public TradingService {
 public:
  [[nodiscard]] base::Status Init(
      const TradingConfig&, const md::MarketDataConfig&) noexcept override {
    return {base::ErrorCode::kUnsupported, {}};
  }
  [[nodiscard]] base::Status Stop() noexcept override { return {}; }
  void Reset() noexcept override {
    accept_requests_ = false;
    stopping_ = false;
  }
  void SetAcceptingRequests(bool accepting) noexcept override {
    accept_requests_ = accepting && !stopping_;
  }
  void BeginStopping() noexcept override {
    accept_requests_ = false;
    stopping_ = true;
  }
  bool accepting_orders() const noexcept override { return false; }
  [[nodiscard]] base::ErrorCode SubmitOrder(
      const model::OrderRequest&, model::ClientOrderId&) noexcept override {
    return RequestError();
  }
  [[nodiscard]] base::ErrorCode CancelOrder(
      model::ClientOrderId) noexcept override {
    return RequestError();
  }
  bool TryGetOrder(model::ClientOrderId,
                   model::OrderSnapshot&) const noexcept override {
    return false;
  }
  bool TryGetCash(model::CashSnapshot&) const noexcept override {
    return false;
  }
  bool TryGetPosition(model::InstrumentId,
                      model::PositionSnapshot&) const noexcept override {
    return false;
  }
  [[nodiscard]] base::Status ProcessRequests(
      std::uint64_t, std::size_t = 64) noexcept override {
    return {};
  }
  [[nodiscard]] base::Status ScheduleStopping(
      std::uint64_t, std::size_t = 64) noexcept override {
    return {};
  }
  std::optional<std::uint64_t> next_deadline() const noexcept override {
    return {};
  }
  bool has_ready_event(std::uint64_t) const noexcept override { return false; }
  bool is_drained() const noexcept override { return true; }
  std::size_t order_count() const noexcept override { return 0; }
  const char* error_stage() const noexcept override {
    return "trading_prepare";
  }
  const char* error_detail() const noexcept override {
    return "trading is disabled";
  }
  [[nodiscard]] base::Status Poll(std::uint64_t, TradingDispatch&,
                                  bool& event) noexcept override {
    event = false;
    return {};
  }

 private:
  base::ErrorCode RequestError() const noexcept {
    return accept_requests_ ? base::ErrorCode::kUnsupported
                            : base::ErrorCode::kInvalidState;
  }
  bool accept_requests_ = false;
  bool stopping_ = false;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_DISABLED_TRADING_RUNTIME_H_
