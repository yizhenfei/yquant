#ifndef YQUANT_TESTS_SCENARIOS_FUNDS_LIFECYCLE_DEMO_H_
#define YQUANT_TESTS_SCENARIOS_FUNDS_LIFECYCLE_DEMO_H_

namespace yquant::verification {

// Verifies partial-fill booking, canceled-remainder release and close profit.
// Returns zero when all client-observed balances match the scenario.
int RunFundsLifecycleDemo();

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_FUNDS_LIFECYCLE_DEMO_H_
