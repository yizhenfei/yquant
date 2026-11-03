#ifndef YQUANT_MONOD_FUNDS_LIFECYCLE_DEMO_H_
#define YQUANT_MONOD_FUNDS_LIFECYCLE_DEMO_H_

namespace yquant::monod {

// Verifies partial-fill booking, canceled-remainder release and close profit.
// Returns zero when all client-observed balances match the scenario.
int RunFundsLifecycleDemo();

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_FUNDS_LIFECYCLE_DEMO_H_
