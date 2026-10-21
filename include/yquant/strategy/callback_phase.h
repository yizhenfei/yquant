#ifndef YQUANT_STRATEGY_CALLBACK_PHASE_H_
#define YQUANT_STRATEGY_CALLBACK_PHASE_H_

namespace yquant::strategy {

// Strategy-host lifecycle phase. The host enters/exits phases serially and
// uses them to gate market subscriptions and trading request acceptance.
enum class CallbackPhase {
  kNone,
  kStart,
  kTick,
  kOrderUpdate,
  kFill,
  kTimer,
  kStop
};

}  // namespace yquant::strategy
#endif  // YQUANT_STRATEGY_CALLBACK_PHASE_H_
