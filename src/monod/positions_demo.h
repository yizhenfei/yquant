#ifndef YQUANT_MONOD_POSITIONS_DEMO_H_
#define YQUANT_MONOD_POSITIONS_DEMO_H_

namespace yquant::monod {

// Queries two account snapshots, then verifies replacement and empty holdings.
// Returns 0 when every field and query result matches the controlled input.
int RunPositionsDemo();

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_POSITIONS_DEMO_H_
