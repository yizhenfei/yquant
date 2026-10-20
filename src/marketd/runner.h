#ifndef YQUANT_MARKETD_RUNNER_H_
#define YQUANT_MARKETD_RUNNER_H_

#include <array>
#include <cstdint>
#include <memory>

#include "marketd/config.h"
#include "marketd/pipeline.h"
#include "strategyd/event_loop_environment.h"
#include "strategyd/monotonic_clock.h"

namespace yquant::marketd {

// Owns a configuration-assembled Source -> Publisher[] pipeline. All methods
// run on the daemon thread; no background worker is created. Each loop services
// every output and polls at most 64 quotes before checking termination again.
class MarketdRunner {
 public:
  MarketdRunner() noexcept = default;
  ~MarketdRunner() noexcept;
  MarketdRunner(const MarketdRunner&) = delete;
  MarketdRunner& operator=(const MarketdRunner&) = delete;

  // Validates all configuration before opening any output. Failure closes all
  // resources acquired by this attempt and preserves an owned diagnostic.
  // now uses the same process-relative clock later passed to Run().
  [[nodiscard]] base::Status Init(const MarketdConfig& config,
                                  std::uint64_t now) noexcept;
  // Requires successful Init. max_events=0 means unlimited; otherwise stops
  // after publishing exactly that many quotes to every output. Any source or
  // publisher failure terminates the pipeline. All exits close all resources.
  [[nodiscard]] base::Status Run(const strategyd::MonotonicClock& clock,
                                 strategyd::EventLoopEnvironment& environment,
                                 std::uint64_t max_events = 0) noexcept;
  // Idempotent; preserves counters and failure diagnostics.
  void Close() noexcept;
  std::uint64_t event_count() const noexcept { return event_count_; }
  const char* error_stage() const noexcept { return error_stage_; }
  const char* error_detail() const noexcept { return error_detail_; }

 private:
  // Copies failure context before cleanup can invalidate adapter diagnostics.
  base::Status Fail(base::Status status, const char* stage,
                    const char* detail) noexcept;

  std::unique_ptr<Source> source_;
  std::array<std::unique_ptr<Publisher>, kMaxPublishers> publishers_;
  std::size_t publisher_count_ = 0;
  std::uint64_t event_count_ = 0;
  char error_stage_[64] = {};
  char error_detail_[768] = {};
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_RUNNER_H_
