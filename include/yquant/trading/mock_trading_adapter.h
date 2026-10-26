#ifndef YQUANT_TRADING_MOCK_TRADING_ADAPTER_H_
#define YQUANT_TRADING_MOCK_TRADING_ADAPTER_H_

#include <stdint.h>

#include <memory>
#include <optional>

#include "yquant/model/trading_event.h"
#include "yquant/trading/trading_adapter.h"

namespace yquant::trading {

// Single-thread timed counter. Prepare allocates fixed storage once; sends and
// polling perform no allocation or waits. Reset starts another session.
class MockTradingAdapter final : public TradingAdapter {
 public:
  MockTradingAdapter() noexcept;
  ~MockTradingAdapter() noexcept override;
  MockTradingAdapter(const MockTradingAdapter&) = delete;
  MockTradingAdapter& operator=(const MockTradingAdapter&) = delete;

  [[nodiscard]] base::Status Prepare(
      const TradingConfig& config) noexcept override;
  [[nodiscard]] base::Status Start(uint64_t now) noexcept override;
  [[nodiscard]] base::Status ReadAccountSnapshot(
      TradingAccountSnapshot& out) noexcept override;
  [[nodiscard]] base::Status SendOrder(model::ClientOrderId id,
                                       const model::OrderRequest& request,
                                       uint64_t now) noexcept override;
  [[nodiscard]] base::Status SendCancel(
      model::ClientOrderId id, const model::AdapterOrderId& adapter_order_id,
      model::CancelRequestId cancel_request_id, uint64_t now) noexcept override;
  [[nodiscard]] TradingPollResult Poll(
      uint64_t now, model::TradingEvent& out) noexcept override;
  std::optional<uint64_t> next_deadline() const noexcept override;
  bool is_drained() const noexcept override;
  [[nodiscard]] base::Status Stop() noexcept override;
  // Stops the session and discards its state and diagnostics. Allows
  // Prepare to begin a new session; all operations require serial access.
  void Reset() noexcept;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  struct Storage;
  struct PreparedReport;
  // Builds the next scheduled event without changing orders or queued plans.
  base::Status PrepareScheduledReport(uint64_t now,
                                      PreparedReport& report) noexcept;
  // Builds one incremental fill and its subsequent scheduled step.
  base::Status PrepareFillReport(uint64_t now, PreparedReport& report) noexcept;
  // Publishes a validated event and advances the indexed plan heap.
  void CommitScheduledReport(uint64_t now,
                             const PreparedReport& report) noexcept;
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

}  // namespace yquant::trading

#endif  // YQUANT_TRADING_MOCK_TRADING_ADAPTER_H_
