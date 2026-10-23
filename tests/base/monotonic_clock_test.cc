#include "yquant/base/monotonic_clock.h"

#include <stdint.h>

#include "gtest/gtest.h"

namespace yquant::base {
namespace {

TEST(MonotonicClockTest, ConsecutiveReadingsNeverDecrease) {
  const base::SteadyMonotonicClock clock;
  const base::MonotonicClock& time_source = clock;
  auto previous = time_source.Now();
  for (int i = 0; i < 1000; ++i) {
    const auto current = time_source.Now();
    EXPECT_GE(current, previous);
    previous = current;
  }
}

TEST(MonotonicClockTest, EachInstanceMeasuresFromItsOwnConstruction) {
  const base::SteadyMonotonicClock first;
  uint64_t age_before_second = 0;
  for (int i = 0; i < 1000; ++i) {
    age_before_second = first.Now();
  }
  ASSERT_GT(age_before_second, 0);

  const base::SteadyMonotonicClock second;
  const auto second_age = second.Now();
  const auto first_age = first.Now();
  // Sampling the older clock last keeps this valid even if execution pauses.
  // The origin difference must include all time before second was constructed.
  ASSERT_GE(first_age, second_age);
  EXPECT_GE(first_age - second_age, age_before_second);
}

}  // namespace
}  // namespace yquant::base
