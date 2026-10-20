#ifndef YQUANT_MARKETD_PIPELINE_H_
#define YQUANT_MARKETD_PIPELINE_H_

#include <cstdint>

#include "yquant/base/status.h"
#include "yquant/model/quote_tick.h"

namespace yquant::marketd {

enum class PollState { kEvent, kIdle, kError };

// Non-event results preserve the caller's quote; failures own no borrowed data.
struct PollResult {
  PollState state = PollState::kIdle;
  base::Status status;
};

// One serialized input to the daemon. Concrete adapters prepare before the
// runner starts, copy complete quotes, and perform bounded nonblocking polls.
class Source {
 public:
  virtual ~Source() noexcept = default;
  [[nodiscard]] virtual PollResult Poll(std::uint64_t now,
                                        model::QuoteTick& out) noexcept = 0;
  virtual void Close() noexcept = 0;
  virtual const char* error_detail() const noexcept = 0;
};

// One serialized output; all buffers are allocated during adapter setup.
// Publish borrows quote only for the call. Service performs bounded control or
// network work, including accepting new clients when there are no events.
class Publisher {
 public:
  virtual ~Publisher() noexcept = default;
  [[nodiscard]] virtual base::Status Publish(
      const model::QuoteTick& quote) noexcept = 0;
  [[nodiscard]] virtual base::Status Service(std::uint64_t now) noexcept = 0;
  virtual void Close() noexcept = 0;
  virtual const char* error_detail() const noexcept = 0;
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_PIPELINE_H_
