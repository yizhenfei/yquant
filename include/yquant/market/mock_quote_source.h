#ifndef YQUANT_MARKET_MOCK_QUOTE_SOURCE_H_
#define YQUANT_MARKET_MOCK_QUOTE_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <optional>

#include "yquant/market/mock_quote_config.h"
#include "yquant/model/quote_tick.h"

namespace yquant::market {

enum class MockQuotePollState { kEvent, kIdle, kError };
struct MockQuotePollResult {
  MockQuotePollState state = MockQuotePollState::kIdle;
  base::Status status;
};

// Single-thread-owned quote generator. Emits every configured instrument in
// configuration order, one quote per Poll. Owns all seeds and sequence state;
// creates no thread and performs no waits or hot-path allocations.
class MockQuoteSource final {
 public:
  MockQuoteSource() = default;
  MockQuoteSource(const MockQuoteSource&) = delete;
  MockQuoteSource& operator=(const MockQuoteSource&) = delete;

  // Validates and copies seeds without generating data. Requires a new object;
  // failure leaves it unprepared, allowing another Prepare attempt.
  [[nodiscard]] base::Status Prepare(const MockQuoteConfig& config) noexcept;
  // Starts every configured instrument one interval after now. Requires a
  // prepared object; failure preserves state and the current deadline.
  [[nodiscard]] base::Status Start(uint64_t now) noexcept;
  // Copies one due quote on kEvent; kIdle/kError preserve out. Each completed
  // batch schedules the next from this call's now. Time/sequence exhaustion
  // stops generation and clears the deadline until Stop.
  [[nodiscard]] MockQuotePollResult Poll(uint64_t now,
                                         model::QuoteTick& out) noexcept;
  std::optional<uint64_t> next_deadline() const noexcept { return deadline_; }
  // Idempotently stops generation and clears scheduling; preserves diagnostics.
  [[nodiscard]] base::Status Stop() noexcept;
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  enum class State { kUnprepared, kPrepared, kActive, kStopped, kFailed };

  // Copies the diagnostic and returns code without changing state or output.
  base::Status Error(base::ErrorCode code, const char* detail) noexcept;
  // Records terminal polling failure, clears the deadline, and keeps out
  // intact.
  MockQuotePollResult PollError(base::ErrorCode code,
                                const char* detail) noexcept;

  State state_ = State::kUnprepared;
  MockQuoteConfig config_;
  std::array<uint64_t, kMaxMockQuoteInstruments> sequences_ = {};
  size_t next_index_ = 0;
  bool shifted_batch_ = false;
  std::optional<uint64_t> deadline_;
  char error_detail_[768] = {};
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_MOCK_QUOTE_SOURCE_H_
