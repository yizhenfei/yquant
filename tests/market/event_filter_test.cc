#include "yquant/market/event_filter.h"

#include <stdint.h>

#include <array>

#include "gtest/gtest.h"

namespace yquant::market {
namespace {

TEST(EventFilterTest, UnrestrictedAndEmptySelections) {
  EventHeader header;
  header.instrument_id = 42;
  header.type = 7;
  EXPECT_TRUE(EventFilter{}.Matches(header));
  EXPECT_FALSE((EventFilter{{}, {}, false, true}.Matches(header)));
  EXPECT_FALSE((EventFilter{{}, {}, true, false}.Matches(header)));
}

TEST(EventFilterTest, UnsortedListsAndStatusVisibility) {
  const uint64_t instruments[] = {99, 42};
  const uint32_t types[] = {8, 7};
  const EventFilter filter{instruments, types, false, false};
  EventHeader header;
  header.instrument_id = 42;
  header.type = 7;
  EXPECT_TRUE(filter.Matches(header));
  header.type = 9;
  EXPECT_FALSE(filter.Matches(header));
  header.scope = EventScope::kInstrumentStatus;
  EXPECT_TRUE(filter.Matches(header));
  header.instrument_id = 100;
  EXPECT_FALSE(filter.Matches(header));
  header.scope = EventScope::kStreamStatus;
  EXPECT_TRUE(filter.Matches(header));
  EXPECT_TRUE((EventFilter{{}, {}, false, false}.Matches(header)));
}

TEST(EventFilterTest, CallerChoosesCapacity) {
  std::array<uint64_t, 128> instruments{};
  instruments.back() = 42;
  const EventFilter filter{instruments, {}, false, true};
  EventHeader header;
  header.instrument_id = 42;
  EXPECT_TRUE(filter.Matches(header));
  header.instrument_id = 99;
  EXPECT_FALSE(filter.Matches(header));
}

}  // namespace
}  // namespace yquant::market
