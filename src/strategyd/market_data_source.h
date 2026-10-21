#ifndef YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_

#include <cstdint>
#include <optional>
#include <span>

#include "strategyd/market_data_config.h"
#include "yquant/base/status.h"
#include "yquant/model/quote_tick.h"

namespace yquant::strategyd {

enum class MarketDataPollState { kEvent, kIdle, kError };

// kEvent writes one complete quote; kIdle/kError leave the caller's quote
// unchanged. status is successful except for kError.
struct MarketDataPollResult {
  MarketDataPollState state = MarketDataPollState::kIdle;
  base::Status status;
};

// Internal host boundary, called serially on the main thread. Sources never
// invoke strategies, sleep in Poll(), or publish borrowed SDK buffers.
class MarketDataSource {
 public:
  virtual ~MarketDataSource() noexcept = default;

  // Copies validated configuration; does not activate or publish quotes.
  // A failed call leaves a new source unprepared and safe to Stop().
  [[nodiscard]] virtual base::Status Prepare(
      const MarketDataConfig& config) noexcept = 0;
  // Copies subscriptions and schedules from the host's monotonic now. Failure
  // retains prepared state; no references to input remain after return.
  [[nodiscard]] virtual base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      std::uint64_t now) noexcept = 0;
  // Nonblocking, bounded work, no strategy callbacks or allocation. Poll only
  // after Start and before Stop. now shares the context's monotonic domain.
  [[nodiscard]] virtual MarketDataPollResult Poll(
      std::uint64_t now, model::QuoteTick& out) noexcept = 0;
  // Empty means idle without a scheduled event; <= current time means ready.
  virtual std::optional<std::uint64_t> next_deadline() const noexcept = 0;
  // Poll-driven transports may remain idle at an already-due deadline. The
  // host checks termination between bounded batches, including empty reads.
  virtual bool is_poll_driven() const noexcept { return false; }
  // Finite sources become finished only after their final quote and optional
  // tail have been consumed by Poll. Live sources remain unfinished.
  virtual bool finished() const noexcept { return false; }
  // Cold maintenance runs once between bounded batches. It may perform I/O or
  // allocate, but must not wait for new data or invoke callbacks. A failure is
  // terminal for the current source; the host calls Stop before returning it.
  [[nodiscard]] virtual base::Status Service(std::uint64_t) noexcept {
    return {};
  }
  // The host includes this deadline in its wait, even without subscriptions.
  virtual std::optional<std::uint64_t> next_service_deadline() const noexcept {
    return std::nullopt;
  }
  // Idempotent synchronous cleanup, safe before activation and after errors.
  // Must end all borrowed input access even when returning an error.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  // Host-owned diagnostic text, valid until the next operation. Stop preserves
  // a preceding source failure so cleanup cannot erase its reason.
  virtual const char* error_detail() const noexcept = 0;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_
