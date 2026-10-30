#ifndef YQUANT_MONOD_RESERVATION_DEMO_H_
#define YQUANT_MONOD_RESERVATION_DEMO_H_

namespace yquant::monod {

// Verifies shared-account checks and send-failure rollback.
// Returns 0 when the controlled scenario produces the expected results.
int RunReservationDemo();

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_RESERVATION_DEMO_H_
