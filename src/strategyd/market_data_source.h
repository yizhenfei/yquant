#ifndef YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_
#define YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_

// Defines the host's market-source lifecycle and polling results. Sources
// provide owned quote values and scheduling hints; strategy callbacks and
// subscription registration belong to the runner and its market-data view.

#include <stdint.h>

#include <optional>
#include <span>

#include "yquant/base/status.h"
#include "yquant/market/market_data_config.h"
#include "yquant/model/quote_tick.h"

namespace yquant::strategyd {

// Separates a delivered quote from a source yield or an operation failure.
enum class MarketDataPollState { kEvent, kIdle, kError };

// Describes one Poll result. Only kEvent overwrites the caller's quote;
// kIdle and kError preserve it. status is non-OK exactly when state is kError.
struct MarketDataPollResult {
  MarketDataPollState state = MarketDataPollState::kIdle;
  base::Status status;
};

// Supplies quotes to the runner after Prepare and Start. The host serializes
// every call on the owning strategy thread and calls Stop after any startup
// attempt, including failures. A source never invokes strategy callbacks.
//
// Poll performs bounded, nonblocking work without allocation; Service handles
// maintenance outside event batches. Scheduling and receive times use host
// monotonic nanoseconds; exchange event timestamps retain their source domain.
class MarketDataSource {
 public:
  virtual ~MarketDataSource() noexcept = default;

  // Prepares from config without activating delivery. Borrows config only for
  // this call; any data needed later must be copied or loaded into owned
  // storage. Failure leaves a new source unprepared and safe to Stop.
  [[nodiscard]] virtual base::Status Prepare(
      const market::MarketDataConfig& config) noexcept = 0;
  // Activates a prepared source with a copied subscription set and scheduling
  // origin now. Failure preserves prepared state. Retains no input references.
  [[nodiscard]] virtual base::Status Start(
      std::span<const model::InstrumentId> subscriptions,
      uint64_t now) noexcept = 0;
  // Polls an active source once, overwriting out only for kEvent. kIdle yields
  // without a quote; kError supplies a non-OK status. The host stops the source
  // after an error. Call only after successful Start and before Stop.
  [[nodiscard]] virtual MarketDataPollResult Poll(
      uint64_t now, model::QuoteTick& out) noexcept = 0;
  // Returns the next absolute polling deadline, or nullopt if none is
  // scheduled. A deadline at or before now makes Poll eligible for the next
  // batch.
  virtual std::optional<uint64_t> next_deadline() const noexcept = 0;
  // Returns whether an idle Poll may leave an already-due deadline. Otherwise
  // an idle source must advance its deadline or remove it. Poll-driven sources
  // are retried in a later batch so the host can check termination first.
  virtual bool is_poll_driven() const noexcept { return false; }
  // Returns whether a finite source has consumed its final quote and tail.
  // Once true, the host begins stopping. Live sources retain the default false.
  virtual bool finished() const noexcept { return false; }
  // Performs maintenance between event batches. May allocate or perform I/O,
  // but must not wait for new data or invoke callbacks. Failure ends the
  // current source session; the host calls Stop during error shutdown.
  [[nodiscard]] virtual base::Status Service(uint64_t) noexcept { return {}; }
  // Returns the next absolute maintenance deadline, or nullopt if unscheduled.
  // The host includes it in its wait even when no instruments are subscribed.
  virtual std::optional<uint64_t> next_service_deadline() const noexcept {
    return std::nullopt;
  }
  // Releases source resources synchronously, including after partial startup.
  // Repeated calls are safe. Ends all borrowed input access even on failure.
  [[nodiscard]] virtual base::Status Stop() noexcept = 0;
  // Returns non-null, source-owned diagnostic text, possibly empty. Contents
  // may change on the next operation; Stop preserves a preceding failure.
  virtual const char* error_detail() const noexcept = 0;
};

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MARKET_DATA_SOURCE_H_
