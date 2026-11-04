#ifndef YQUANT_TESTS_SCENARIOS_POSITIONS_DEMO_H_
#define YQUANT_TESTS_SCENARIOS_POSITIONS_DEMO_H_

namespace yquant::verification {

// Queries two account snapshots, then verifies replacement and empty holdings.
// Returns 0 when every field and query result matches the controlled input.
int RunPositionsDemo();

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_POSITIONS_DEMO_H_
