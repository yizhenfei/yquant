#ifndef YQUANT_BASE_MONOTONIC_CLOCK_H_
#define YQUANT_BASE_MONOTONIC_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace yquant::base {

// Shared nanosecond time domain for scheduling and elapsed-time measurements.
// Implementations must be monotonic, allocation-free, and outlive their users.
// All users that compare timestamps must use the same clock instance.
class MonotonicClock {
 public:
  virtual ~MonotonicClock() noexcept = default;
  // Returns nanoseconds in this clock's time domain. The origin is arbitrary;
  // consecutive calls must not decrease, and the call must not allocate.
  virtual std::uint64_t Now() const noexcept = 0;
};

// Measures elapsed nanoseconds from construction, independently of wall time.
class SteadyMonotonicClock final : public MonotonicClock {
 public:
  std::uint64_t Now() const noexcept override {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - origin_)
            .count());
  }

 private:
  static_assert(std::chrono::steady_clock::is_steady);
  std::chrono::steady_clock::time_point origin_ =
      std::chrono::steady_clock::now();
};

}  // namespace yquant::base

#endif  // YQUANT_BASE_MONOTONIC_CLOCK_H_
