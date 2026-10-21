#ifndef YQUANT_STRATEGY_CALLBACK_PHASE_H_
#define YQUANT_STRATEGY_CALLBACK_PHASE_H_

namespace yquant::strategy {

// Host-side lifecycle gate shared by market-data and trading services. The
// caller enters/exits phases serially; no phase dispatches callbacks itself.
enum class CallbackPhase { kNone, kStart, kTick, kOrderUpdate, kFill, kStop };

}  // namespace yquant::strategy
#endif  // YQUANT_STRATEGY_CALLBACK_PHASE_H_
