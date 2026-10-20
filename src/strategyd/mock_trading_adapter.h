#ifndef YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_
#define YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_

#include <cstdint>
#include <memory>
#include <optional>

#include "strategyd/trading_adapter.h"

namespace yquant::strategyd {

// Single-thread timed counter. Prepare allocates fixed storage once; sends and
// polling perform no allocation, waits or callbacks. One session per object.
class MockTradingAdapter final : public TradingAdapter {
 public:
  MockTradingAdapter() noexcept;
  ~MockTradingAdapter() noexcept override;
  MockTradingAdapter(const MockTradingAdapter&) = delete;
  MockTradingAdapter& operator=(const MockTradingAdapter&) = delete;

  [[nodiscard]] base::Status Prepare(
      const TradingConfig& config) noexcept override;
  [[nodiscard]] base::Status Start(std::uint64_t now) noexcept override;
  [[nodiscard]] base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override;
  [[nodiscard]] base::Status SendOrder(model::ClientOrderId id,
                                       const model::OrderRequest& request,
                                       std::uint64_t now) noexcept override;
  [[nodiscard]] base::Status SendCancel(
      model::ClientOrderId id, const model::BrokerOrderId& broker_order_id,
      model::CancelRequestId cancel_request_id,
      std::uint64_t now) noexcept override;
  [[nodiscard]] TradingPollResult Poll(std::uint64_t now,
                                       TradingEvent& out) noexcept override;
  std::optional<std::uint64_t> next_deadline() const noexcept override;
  bool is_drained() const noexcept override;
  [[nodiscard]] base::Status Stop() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  struct Storage;
  enum class State { kUnprepared, kPrepared, kActive, kFailed, kStopped };

  // Copies a failure without changing state or caller output.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Records terminal polling failure; out and pending plans stay unchanged.
  TradingPollResult PollError(base::ErrorCode code,
                              const char* detail) noexcept;

  std::unique_ptr<Storage> storage_;
  State state_ = State::kUnprepared;
  bool drained_on_stop_ = true;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_
