#ifndef YQUANT_TESTS_SCENARIOS_STARTUP_DEMO_H_
#define YQUANT_TESTS_SCENARIOS_STARTUP_DEMO_H_

namespace yquant::verification {

// Verifies startup failure, retry and protection against late snapshots.
// Returns zero when both client queries and the request count are correct.
int RunStartupDemo();

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_STARTUP_DEMO_H_
