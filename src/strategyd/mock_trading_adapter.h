#ifndef YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_
#define YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "strategyd/trading_adapter.h"

namespace yquant::strategyd {

inline constexpr std::size_t kMaxMockTradingStateBytes = 8388608;
inline constexpr std::size_t kMaxMockTradingMutationBytes = 1024;
inline constexpr std::size_t kMaxMockTradingReports = 8192;

enum class MockTradingMutationKind : std::uint8_t {
  kSendOrder = 1,
  kSendCancel = 2,
  kReport = 3
};

// Complete accepted operation, recorded immediately before the mock commits.
// Only kSendOrder uses request/request_key; kSendCancel uses broker_order_id
// and cancel_request_id; kReport owns the exact generated event, including UTC.
struct MockTradingMutation {
  MockTradingMutationKind kind = MockTradingMutationKind::kSendOrder;
  std::uint64_t now = 0;
  model::ClientOrderId client_order_id;
  model::OrderRequest request;
  model::BrokerOrderId request_key;
  model::BrokerOrderId broker_order_id;
  model::CancelRequestId cancel_request_id;
  TradingEvent event;
};

// The callback borrows mutation only for this call, must not reenter the
// adapter, and must complete its journal copy before returning. A failure
// prevents the operation from changing mock state or publishing a report.
using MockTradingMutationObserver = base::Status (*)(
    void* context, const MockTradingMutation& mutation) noexcept;

// Cash-only versioned little-endian journal representation. Non-kNone
// position effects are rejected; no native struct bytes are serialized.
// Failure preserves used/out; an encoding failure may modify the output span.
[[nodiscard]] base::Status EncodeMockTradingMutation(
    const MockTradingMutation& mutation, std::span<std::byte> output,
    std::size_t& used) noexcept;
[[nodiscard]] base::Status DecodeMockTradingMutation(
    std::span<const std::byte> input, MockTradingMutation& out) noexcept;

// Single-thread timed counter. Prepare allocates fixed storage once; sends and
// polling perform no allocation or waits. An installed mutation observer runs
// synchronously before each commit. One session per object.
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

  // Installs a borrowed observer/context until replaced or Stop. The caller
  // owns both and serializes changes with every other adapter operation.
  void SetMutationObserver(MockTradingMutationObserver observer,
                           void* context) noexcept;
  // Optional recoverable external key, copied into the order before acceptance.
  // An empty key retains the normal session-local mock identity. Nonempty keys
  // must be unique and contain at most 64 bytes. Nonempty keys or an installed
  // observer require a cash account and equity requests with kNone position
  // effect. Failure leaves state unchanged.
  [[nodiscard]] base::Status SendOrderWithIdentity(
      model::ClientOrderId id, const model::OrderRequest& request,
      std::string_view request_key, std::uint64_t now) noexcept;

  // Cold-path portable snapshot; now is the clock at capture/restore. Scheduled
  // deadlines retain remaining delays (overdue work becomes due at now).
  // Only cash accounts and kNone position effects can be persisted.
  // Restore requires Prepare/Start and compatible configuration, validates a
  // private candidate, and preserves existing state and saved_now on failure.
  // used changes only on success. Encoding may modify output before failure.
  [[nodiscard]] base::Status EncodeState(std::span<std::byte> output,
                                         std::size_t& used,
                                         std::uint64_t now) const noexcept;
  [[nodiscard]] base::Status RestoreState(
      std::span<const std::byte> input, std::uint64_t now,
      std::uint64_t* saved_now = nullptr) noexcept;
  // Replays one accepted mutation without invoking the observer or generating
  // new UTC timestamps. Use recorded monotonic times throughout replay, then
  // rebase once. Corrupt/invalid input leaves state unchanged.
  [[nodiscard]] base::Status ReplayMutation(
      const MockTradingMutation& mutation) noexcept;
  // Atomically shifts remaining plan delays to a new monotonic clock. Fails
  // without changing deadlines if any new deadline would overflow.
  [[nodiscard]] base::Status RebaseDeadlines(std::uint64_t old_now,
                                             std::uint64_t new_now) noexcept;

  // Cold-path complete local history. These facts cover the recovered prefix;
  // the in-process mock supplies no independent evidence for a lost WAL tail.
  // Invalid indices preserve output. request_key may be omitted.
  std::size_t recovery_order_count() const noexcept;
  std::size_t recovery_fill_count() const noexcept;
  std::size_t recovery_event_count() const noexcept;
  [[nodiscard]] base::Status ReadRecoveryOrder(
      std::size_t index, model::OrderSnapshot& out,
      model::BrokerOrderId* request_key = nullptr) const noexcept;
  [[nodiscard]] base::Status ReadRecoveryFill(std::size_t index,
                                              model::Fill& out) const noexcept;
  [[nodiscard]] base::Status ReadRecoveryEvent(
      std::size_t index, TradingEvent& out) const noexcept;

 private:
  struct Storage;
  enum class State { kUnprepared, kPrepared, kActive, kFailed, kStopped };

  // Copies a failure without changing state or caller output.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Records terminal polling failure; out and pending plans stay unchanged.
  TradingPollResult PollError(base::ErrorCode code,
                              const char* detail) noexcept;
  // Poll implementation optionally checks an exact replay event before commit;
  // replay bypasses the observer and uses its recorded UTC fill timestamp.
  TradingPollResult PollImpl(std::uint64_t now, TradingEvent& out,
                             const TradingEvent* replay) noexcept;
  base::Status Observe(const MockTradingMutation& mutation) noexcept;

  std::unique_ptr<Storage> storage_;
  State state_ = State::kUnprepared;
  bool drained_on_stop_ = true;
  MockTradingMutationObserver observer_ = nullptr;
  void* observer_context_ = nullptr;
  char error_detail_[768] = {};
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MOCK_TRADING_ADAPTER_H_
