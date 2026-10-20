#ifndef YQUANT_STRATEGYD_MONOTONIC_CLOCK_H_
#define YQUANT_STRATEGYD_MONOTONIC_CLOCK_H_

#include <chrono>
#include <cstdint>

namespace yquant::strategyd {

// Shared time domain for the context, source scheduling, and event loop.
// Implementations must be monotonic, allocation-free, and outlive their users.
class MonotonicClock {
 public:
  virtual ~MonotonicClock() noexcept = default;
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

}  // namespace yquant::strategyd

#endif  // YQUANT_STRATEGYD_MONOTONIC_CLOCK_H_
