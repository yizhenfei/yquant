#include <stdint.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <new>

#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

#ifndef YQUANT_TEST_TIMER_MODE
#define YQUANT_TEST_TIMER_MODE 0
#endif

namespace {

using yquant::base::ErrorCode;
using yquant::model::ClientOrderId;
using yquant::model::Fill;
using yquant::model::InstrumentDefinition;
using yquant::model::OrderRequest;
using yquant::model::OrderSide;
using yquant::model::OrderSnapshot;
using yquant::model::OrderState;
using yquant::model::OrderUpdate;
using yquant::model::OrderUpdateKind;
using yquant::model::QuoteTick;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyContext;
using yquant::strategy::StrategyPlugin;
using yquant::strategy::TimerEvent;

// Pipe markers expose ordering without retaining callback inputs or services.
void Emit(char event) noexcept {
  const char* text = std::getenv("YQUANT_TEST_EVENT_FD");
  if (text == nullptr) {
    return;
  }
  const int fd = static_cast<int>(std::strtol(text, nullptr, 10));
  while (write(fd, &event, 1) == -1 && errno == EINTR) {
  }
}

class TimerFixture final : public Strategy {
 public:
  StrategyConfig& config() noexcept override { return config_; }

  ErrorCode OnStart(StrategyContext& context) noexcept override {
    if (context.timer().deadline()) {
      return ErrorCode::kCorrupt;
    }
    if constexpr (YQUANT_TEST_TIMER_MODE >= 4 && YQUANT_TEST_TIMER_MODE <= 6) {
      const auto found =
          context.market_data().FindInstrument("SSE", "600000", instrument_);
      if (found != ErrorCode::kOk) {
        return found;
      }
      if constexpr (YQUANT_TEST_TIMER_MODE == 6) {
        const auto subscribed =
            context.market_data().SubscribeQuotes(instrument_.id);
        if (subscribed != ErrorCode::kOk) {
          return subscribed;
        }
      }
    }
    Emit('S');
    if constexpr (YQUANT_TEST_TIMER_MODE == 0) {
      return context.timer().Arm(7000000);
    }
    if constexpr (YQUANT_TEST_TIMER_MODE == 1) {
      if (context.timer().Arm(1000000) != ErrorCode::kOk) {
        return ErrorCode::kCorrupt;
      }
      return context.timer().Cancel();
    }
    if constexpr (YQUANT_TEST_TIMER_MODE == 2) {
      if (context.timer().Arm(1000000) != ErrorCode::kOk) {
        return ErrorCode::kCorrupt;
      }
      return context.timer().Arm(3000000);
    }
    if constexpr (YQUANT_TEST_TIMER_MODE == 6 || YQUANT_TEST_TIMER_MODE == 7) {
      return context.timer().Arm(0);
    }
    return context.timer().Arm(1000000);
  }

  ErrorCode OnTick(StrategyContext&, const QuoteTick&) noexcept override {
    if (awaiting_fill_) {
      return ErrorCode::kCorrupt;
    }
    Emit('Q');
    return ErrorCode::kOk;
  }

  ErrorCode OnTimer(StrategyContext& context,
                    const TimerEvent& event) noexcept override {
    if (context.timer().deadline() || event.fire_time_ns < event.deadline_ns ||
        event.fire_time_ns > context.Now() || awaiting_fill_) {
      return ErrorCode::kCorrupt;
    }
    ++fires_;
    Emit('H');
    if constexpr (YQUANT_TEST_TIMER_MODE == 3) {
      return fires_ < 3 ? context.timer().Arm(context.Now() + 1000000)
                        : ErrorCode::kOk;
    }
    if constexpr (YQUANT_TEST_TIMER_MODE >= 4 && YQUANT_TEST_TIMER_MODE <= 6) {
      if (order_id_.value == 0) {
        const OrderRequest request{instrument_.id, OrderSide::kBuy, 1001, 1};
        const auto result = context.trading().SubmitOrder(request, order_id_);
        if (result != ErrorCode::kOk) {
          return result;
        }
        OrderSnapshot pending;
        if (!context.trading().TryGetOrder(order_id_, pending) ||
            pending.state != OrderState::kPendingNew) {
          return ErrorCode::kCorrupt;
        }
        Emit('N');
        if constexpr (YQUANT_TEST_TIMER_MODE == 5) {
          return context.timer().Arm(context.Now() + 1000000);
        }
      } else if constexpr (YQUANT_TEST_TIMER_MODE == 4) {
        const auto canceled = context.trading().CancelOrder(order_id_);
        if (canceled != ErrorCode::kOk) {
          return canceled;
        }
        Emit('K');
      } else if constexpr (YQUANT_TEST_TIMER_MODE == 5) {
        return ErrorCode::kInvalidArgument;
      }
      if constexpr (YQUANT_TEST_TIMER_MODE == 6) {
        return context.timer().Arm(context.Now());
      }
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnOrderUpdate(StrategyContext& context,
                          const OrderUpdate& update) noexcept override {
    OrderSnapshot snapshot;
    if (awaiting_fill_ ||
        !context.trading().TryGetOrder(update.order.client_order_id,
                                       snapshot) ||
        snapshot != update.order) {
      return ErrorCode::kCorrupt;
    }
    awaiting_fill_ = update.kind == OrderUpdateKind::kFill;
    if (update.kind == OrderUpdateKind::kAccepted) {
      Emit('A');
      if constexpr (YQUANT_TEST_TIMER_MODE == 4) {
        return context.timer().Arm(context.Now() + 1000000);
      }
    } else if (update.kind == OrderUpdateKind::kFill) {
      Emit('V');
    } else if (update.kind == OrderUpdateKind::kCanceled) {
      Emit('X');
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnFill(StrategyContext&, const Fill&) noexcept override {
    if (!awaiting_fill_) {
      return ErrorCode::kCorrupt;
    }
    awaiting_fill_ = false;
    Emit('F');
    return ErrorCode::kOk;
  }

  ErrorCode OnStop(StrategyContext& context) noexcept override {
    if (context.timer().deadline() ||
        context.timer().Arm(0) != ErrorCode::kInvalidState ||
        context.timer().Cancel() != ErrorCode::kInvalidState) {
      return ErrorCode::kCorrupt;
    }
    if (order_id_.value != 0) {
      OrderSnapshot order;
      if (!context.trading().TryGetOrder(order_id_, order) ||
          !yquant::model::IsTerminalOrderState(order.state) ||
          order.cancel_pending) {
        return ErrorCode::kCorrupt;
      }
      Emit(order.state == OrderState::kFilled ? 'f' : 'c');
    }
    Emit('T');
    return ErrorCode::kOk;
  }

 private:
  StrategyConfig config_;
  InstrumentDefinition instrument_;
  ClientOrderId order_id_;
  uint64_t fires_ = 0;
  bool awaiting_fill_ = false;
};

ErrorCode Create(Strategy*& out) noexcept {
  Emit('C');
  out = new (std::nothrow) TimerFixture;
  return out == nullptr ? ErrorCode::kResourceExhausted : ErrorCode::kOk;
}
void Destroy(Strategy* strategy) noexcept {
  delete strategy;
  Emit('D');
}
const StrategyPlugin kPlugin = {yquant::strategy::kStrategyPluginAbiVersion,
                                static_cast<uint32_t>(sizeof(StrategyPlugin)),
                                Create, Destroy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
