#ifndef YQUANT_MONOD_STARTUP_DEMO_H_
#define YQUANT_MONOD_STARTUP_DEMO_H_

namespace yquant::monod {

// Verifies startup failure, retry and protection against late snapshots.
// Returns zero when both client queries and the request count are correct.
int RunStartupDemo();

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_STARTUP_DEMO_H_
