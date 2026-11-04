#ifndef YQUANT_TESTS_SCENARIOS_TRADING_DEMO_H_
#define YQUANT_TESTS_SCENARIOS_TRADING_DEMO_H_

namespace yquant::verification {

enum class TradingScenario { kSubmit, kCancel, kFill };

// Runs a controlled trading scenario; returns 0 when its results are correct.
int RunTradingDemo(TradingScenario scenario = TradingScenario::kSubmit);

}  // namespace yquant::verification

#endif  // YQUANT_TESTS_SCENARIOS_TRADING_DEMO_H_
