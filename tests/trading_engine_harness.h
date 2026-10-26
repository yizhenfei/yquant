#ifndef YQUANT_TESTS_TRADING_ENGINE_HARNESS_H_
#define YQUANT_TESTS_TRADING_ENGINE_HARNESS_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <utility>

#include "yquant/strategy/callback_phase.h"
#include "yquant/trading/trading_engine.h"

namespace yquant::testing {

// Adapts ledger transaction tests to an engine-owned adapter. It creates no
// strategy service and performs no real transport. Production uses Init.
class TradingEngineHarness {
 public:
  TradingEngineHarness(const base::MonotonicClock& clock,
                       const md::InstrumentScope& instruments) noexcept
      : engine_(&clock, &instruments, &adapter_) {}
  base::Status Configure(
      const trading::TradingAccountSnapshot& account) noexcept {
    adapter_.initial = account;
    trading::TradingConfig config;
    config.initial_account = account;
    config.adapter = account.type == model::AccountType::kFutures
                         ? trading::TradingAdapterKind::kSimulatedFutures
                         : trading::TradingAdapterKind::kMock;
    auto status = engine_.Init(config, {});
    if (!status.ok()) {
      engine_.Reset();
    }
    return status;
  }
  void SetRunning(bool running) noexcept { running_ = running; }
  void BeginCallback(strategy::CallbackPhase phase) noexcept {
    const bool business = phase == strategy::CallbackPhase::kTick ||
                          phase == strategy::CallbackPhase::kOrderUpdate ||
                          phase == strategy::CallbackPhase::kFill ||
                          phase == strategy::CallbackPhase::kTimer;
    engine_.SetAcceptingRequests(running_ && business);
  }
  void EndCallback() noexcept { engine_.SetAcceptingRequests(false); }
  base::Status ProcessRequests(trading::TradingAdapter& adapter, uint64_t now,
                               size_t limit = 64) noexcept {
    adapter_.target = &adapter;
    return engine_.ProcessRequests(now, limit);
  }
#define YQUANT_TEST_ENGINE_FORWARD(name)              \
  template <typename... Args>                         \
  decltype(auto) name(Args&&... args) noexcept {      \
    return engine_.name(std::forward<Args>(args)...); \
  }
  base::ErrorCode SubmitOrder(const model::OrderRequest& r,
                              model::ClientOrderId& id) noexcept {
    return engine_.SubmitOrder(r, id);
  }
  base::ErrorCode CancelOrder(model::ClientOrderId id) noexcept {
    return engine_.CancelOrder(id);
  }
  YQUANT_TEST_ENGINE_FORWARD(BeginStopping)
  YQUANT_TEST_ENGINE_FORWARD(ApplyEvent)
  YQUANT_TEST_ENGINE_FORWARD(ApplyLocalUpdate)
  YQUANT_TEST_ENGINE_FORWARD(ApplyQuote)
  YQUANT_TEST_ENGINE_FORWARD(ScheduleStopping)
  YQUANT_TEST_ENGINE_FORWARD(Reset)
#undef YQUANT_TEST_ENGINE_FORWARD
#define YQUANT_TEST_ENGINE_QUERY(name)                 \
  template <typename... Args>                          \
  decltype(auto) name(Args&&... args) const noexcept { \
    return engine_.name(std::forward<Args>(args)...);  \
  }
  bool TryGetOrder(model::ClientOrderId id,
                   model::OrderSnapshot& out) const noexcept {
    return engine_.TryGetOrder(id, out);
  }
  YQUANT_TEST_ENGINE_QUERY(TryGetOrderAt)
  YQUANT_TEST_ENGINE_QUERY(TryGetFillAt)
  YQUANT_TEST_ENGINE_QUERY(TryGetCash)
  YQUANT_TEST_ENGINE_QUERY(TryGetPosition)
  YQUANT_TEST_ENGINE_QUERY(TryGetFuturesAccount)
  YQUANT_TEST_ENGINE_QUERY(TryGetFuturesPosition)
  YQUANT_TEST_ENGINE_QUERY(order_count)
  YQUANT_TEST_ENGINE_QUERY(fill_count)
  YQUANT_TEST_ENGINE_QUERY(next_deadline)
  YQUANT_TEST_ENGINE_QUERY(has_local_updates)
  YQUANT_TEST_ENGINE_QUERY(is_drained)
  YQUANT_TEST_ENGINE_QUERY(accepting_orders)
  YQUANT_TEST_ENGINE_QUERY(error_detail)
#undef YQUANT_TEST_ENGINE_QUERY

 private:
  struct Adapter final : trading::TradingAdapter {
    trading::TradingAccountSnapshot initial;
    trading::TradingAdapter* target = nullptr;
    base::Status Prepare(const trading::TradingConfig&) noexcept override {
      return {};
    }
    base::Status Start(uint64_t) noexcept override { return {}; }
    base::Status ReadAccountSnapshot(
        trading::TradingAccountSnapshot& out) noexcept override {
      out = initial;
      return {};
    }
    base::Status SendOrder(model::ClientOrderId id,
                           const model::OrderRequest& request,
                           uint64_t now) noexcept override {
      return target->SendOrder(id, request, now);
    }
    base::Status SendCancel(model::ClientOrderId id,
                            const model::AdapterOrderId& broker,
                            model::CancelRequestId cancel,
                            uint64_t now) noexcept override {
      return target->SendCancel(id, broker, cancel, now);
    }
    trading::TradingPollResult Poll(
        uint64_t now, model::TradingEvent& out) noexcept override {
      return target ? target->Poll(now, out) : trading::TradingPollResult{};
    }
    std::optional<uint64_t> next_deadline() const noexcept override {
      return target ? target->next_deadline() : std::nullopt;
    }
    bool is_drained() const noexcept override {
      return !target || target->is_drained();
    }
    base::Status Stop() noexcept override { return {}; }
    const char* error_detail() const noexcept override {
      return target ? target->error_detail() : "";
    }
  } adapter_;
  trading::TradingEngine engine_;
  bool running_ = false;
};

}  // namespace yquant::testing
#endif  // YQUANT_TESTS_TRADING_ENGINE_HARNESS_H_
