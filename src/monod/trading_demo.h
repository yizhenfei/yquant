#ifndef YQUANT_MONOD_TRADING_DEMO_H_
#define YQUANT_MONOD_TRADING_DEMO_H_

namespace yquant::monod {

enum class TradingScenario { kSubmit, kCancel, kFill };

// Runs a controlled trading scenario; returns 0 when its results are correct.
int RunTradingDemo(TradingScenario scenario = TradingScenario::kSubmit);

}  // namespace yquant::monod

#endif  // YQUANT_MONOD_TRADING_DEMO_H_
