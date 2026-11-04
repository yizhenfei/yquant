#ifndef YQUANT_TESTS_SCENARIOS_RESERVATION_DEMO_H_
#define YQUANT_TESTS_SCENARIOS_RESERVATION_DEMO_H_

namespace yquant::verification {

// Verifies shared-account checks and send-failure rollback.
// Returns 0 when the controlled scenario produces the expected results.
int RunReservationDemo();

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_RESERVATION_DEMO_H_
