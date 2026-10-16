#ifndef YQUANT_STRATEGY_STRATEGY_H_
#define YQUANT_STRATEGY_STRATEGY_H_

#include "yquant/base/status.h"

namespace yquant::strategy {

// Callbacks execute serially on the host thread and must return promptly.
class Strategy {
 public:
  virtual ~Strategy() noexcept = default;

  // On failure, roll back startup resources and remain safe to destroy.
  [[nodiscard]] virtual base::ErrorCode OnStart() noexcept = 0;

  // Called once after a successful OnStart(). Stop all owned activity before
  // returning, including when reporting an error.
  [[nodiscard]] virtual base::ErrorCode OnStop() noexcept = 0;
};

}  // namespace yquant::strategy

#endif  // YQUANT_STRATEGY_STRATEGY_H_
