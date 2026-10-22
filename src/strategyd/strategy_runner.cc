#include "strategyd/strategy_runner.h"

#include <pthread.h>
#include <signal.h>
#include <sys/select.h>

#include <cerrno>
#include <cstdio>
#include <limits>
#include <system_error>

#include "strategyd/strategy_config_loader.h"
#include "yquant/trading/trading_client_config.h"

namespace yquant::strategyd {

StrategyRunner::StrategyRunner(const base::MonotonicClock& clock,
                               base::EventLoopEnvironment& environment,
                               MarketDataSource& source,
                               trading::TradingService& trading) noexcept
    : clock_(clock),
      environment_(environment),
      source_(source),
      trading_service_(trading),
      market_data_(clock),
      context_(clock, market_data_, trading, timer_) {}

StrategyRunner::~StrategyRunner() noexcept {
  // Explicit Stop reports errors; this is the ownership safety net.
  Finalize();
}

void StrategyRunner::ResetError() noexcept {
  result_ = {};
  error_stage_ = "";
  error_detail_[0] = '\0';
}

void StrategyRunner::SetError(base::Status status, const char* stage,
                              const char* detail) noexcept {
  if (status.ok() || !result_.ok()) {
    return;
  }
  result_ = status;
  error_stage_ = stage;
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
}

void StrategyRunner::SetQuoteError(base::Status status, const char* stage,
                                   const char* detail,
                                   const model::QuoteTick& tick) noexcept {
  char copied[kErrorDetailSize];
  std::snprintf(copied, sizeof(copied),
                "source_id=%u instrument_id=%llu sequence=%llu: %s",
                tick.source_id.value,
                static_cast<unsigned long long>(tick.instrument_id.value),
                static_cast<unsigned long long>(tick.sequence), detail);
  SetError(status, stage, copied);
}

bool StrategyRunner::CancelStartupIfRequested() noexcept {
  bool stop = false;
  const auto status = environment_.CheckStop(stop);
  if (!status.ok()) {
    SetError(status, "signal", "could not inspect termination signals");
  }
  if (stop || !result_.ok()) {
    Finalize();
    return true;
  }
  return false;
}

base::Status StrategyRunner::LoadAndStart(
    StrategyStartOptions options) noexcept {
  ResetError();
  if (state_ != RunnerState::kIdle) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "a strategy can only be loaded in the idle state");
    return result_;
  }

  state_ = RunnerState::kStarting;
  if (CancelStartupIfRequested()) {
    return result_;
  }
  auto& config_loader = options.config;
  strategy_id_ = config_loader.strategy_id();
  // A source may acquire resources before a failed Prepare; always stop it.
  source_prepared_ = true;
  const auto prepared = source_.Prepare(options.market_data);
  if (!prepared.ok()) {
    SetError(prepared, "market_data_prepare", source_.error_detail());
    Finalize();
    return result_;
  }
  const auto configured = market_data_.Configure(options.market_data);
  if (!configured.ok()) {
    SetError(configured, "market_data_prepare",
             "could not prepare the instrument dictionary");
    Finalize();
    return result_;
  }
  if (CancelStartupIfRequested()) return result_;

  shutdown_timeout_ns_ = options.trading_config.shutdown_timeout_ns;
  trading_prepared_ = true;
  const auto initialized =
      trading_service_.Init(options.trading_config, options.market_data);
  if (!initialized.ok()) {
    SetError(initialized, trading_service_.error_stage(),
             trading_service_.error_detail());
    Finalize();
    return result_;
  }
  trading_started_ = true;
  if (CancelStartupIfRequested()) return result_;

  const base::Status open_result = library_.Open(options.library_path);
  if (!open_result.ok()) {
    SetError(open_result, library_.error_stage(), library_.error_detail());
    Finalize();
    return result_;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  const auto* plugin = library_.plugin();
  const base::ErrorCode create_result = plugin->create(instance_);
  if ((create_result == base::ErrorCode::kOk) != (instance_ != nullptr)) {
    SetError({base::ErrorCode::kCorrupt, {}}, "create",
             "strategy factory returned inconsistent status and instance");
  } else if (create_result != base::ErrorCode::kOk) {
    SetError({create_result, {}}, "create",
             "strategy factory returned an error");
  }
  if (!result_.ok()) {
    Finalize();
    return result_;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  base::ConfigError config_error;
  strategy::StrategyConfig& config = instance_->config();
  const base::ErrorCode initialize_result =
      config.Initialize(config_loader.strategy_id(),
                        config_loader.parameters_json(), config_error);
  if (initialize_result != base::ErrorCode::kOk) {
    // Plugins report paths relative to parameters. Copy both parts before
    // destroying the instance or releasing its library.
    char detail[kErrorDetailSize];
    const char* separator = config_error.field_path[0] == '\0' ? "" : ".";
    std::snprintf(detail, sizeof(detail), "parameters%s%s: %s%s", separator,
                  config_error.field_path,
                  config_error.message[0] != '\0'
                      ? config_error.message
                      : "strategy configuration initialization failed",
                  config_error.truncated ? " [truncated]" : "");
    SetError({initialize_result, {}}, "initialize", detail);
    config_loader.Reset();
    Finalize();
    return result_;
  }
  if (!config.ready()) {
    SetError({base::ErrorCode::kCorrupt, {}}, "initialize",
             "strategy configuration did not become ready");
    config_loader.Reset();
    Finalize();
    return result_;
  }
  // JSON views are initialization-only. OnStart must use owned typed fields.
  config_loader.Reset();

  if (CancelStartupIfRequested()) {
    return result_;
  }
  BeginCallback(strategy::CallbackPhase::kStart);
  const base::ErrorCode start_result = instance_->OnStart(context_);
  EndCallback();
  if (start_result != base::ErrorCode::kOk) {
    SetError({start_result, {}}, "start", "strategy OnStart returned an error");
    Finalize();
    return result_;
  }
  started_ = true;
  if (CancelStartupIfRequested()) {
    return result_;
  }
  if (source_prepared_) {
    const auto status =
        source_.Start(market_data_.subscriptions(), clock_.Now());
    if (!status.ok()) {
      SetError(status, "market_data_start", source_.error_detail());
      Finalize();
      return result_;
    }
    source_started_ = true;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }
  state_ = RunnerState::kRunning;
  timer_.SetRunning(true);
  return result_;
}

base::Status StrategyRunner::Stop() noexcept {
  if (callback_active_ || state_ == RunnerState::kStarting) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  if (state_ == RunnerState::kStopping) {
    return {};
  }
  ResetError();
  if (state_ == RunnerState::kIdle) {
    Finalize();
  } else if (state_ == RunnerState::kRunning) {
    stop_requested_ = true;
    timer_.BeginStopping();
  }
  return result_;
}

namespace {

// Both sources use the host's monotonic time domain.
bool IsDue(std::optional<std::uint64_t> deadline, std::uint64_t now) noexcept {
  return deadline && *deadline <= now;
}

// Keeps the earliest absolute deadline; absent inputs do not schedule work.
void IncludeDeadline(std::optional<std::uint64_t>& earliest,
                     std::optional<std::uint64_t> candidate) noexcept {
  if (candidate && (!earliest || *candidate < *earliest)) {
    earliest = candidate;
  }
}

}  // namespace

std::optional<std::uint64_t> StrategyRunner::next_deadline() const noexcept {
  if (state_ != RunnerState::kRunning && state_ != RunnerState::kStopping) {
    return std::nullopt;
  }
  std::optional<std::uint64_t> deadline;
  if (stop_requested_) {
    deadline = 0;
  }
  if (state_ == RunnerState::kRunning && source_started_) {
    IncludeDeadline(deadline, source_.next_deadline());
    IncludeDeadline(deadline, source_.next_service_deadline());
  }
  if (state_ == RunnerState::kRunning) {
    IncludeDeadline(deadline, timer_.deadline());
  }
  if (trading_started_) {
    IncludeDeadline(deadline, trading_service_.next_deadline());
  }
  IncludeDeadline(deadline, shutdown_deadline_);
  return deadline;
}

void StrategyRunner::BeginCallback(strategy::CallbackPhase phase) noexcept {
  callback_active_ = true;
  market_data_.BeginCallback(phase);
  timer_.BeginCallback(phase);
  const bool business = phase == strategy::CallbackPhase::kTick ||
                        phase == strategy::CallbackPhase::kOrderUpdate ||
                        phase == strategy::CallbackPhase::kFill ||
                        phase == strategy::CallbackPhase::kTimer;
  trading_service_.SetAcceptingRequests(state_ == RunnerState::kRunning &&
                                        business);
}

void StrategyRunner::EndCallback() noexcept {
  timer_.EndCallback();
  trading_service_.SetAcceptingRequests(false);
  market_data_.EndCallback();
  callback_active_ = false;
}

void StrategyRunner::SetOrderError(base::Status status, const char* stage,
                                   const char* detail, model::ClientOrderId id,
                                   std::uint64_t sequence) noexcept {
  char copied[kErrorDetailSize];
  std::snprintf(copied, sizeof(copied),
                "client_order_id=%llu sequence=%llu: %s",
                static_cast<unsigned long long>(id.value),
                static_cast<unsigned long long>(sequence), detail);
  SetError(status, stage, copied);
}

void StrategyRunner::EnterStopping() noexcept {
  if (state_ == RunnerState::kStopping || state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  stop_requested_ = false;
  trading_service_.BeginStopping();
  timer_.BeginStopping();
  const auto now = clock_.Now();
  if (shutdown_timeout_ns_ > std::numeric_limits<std::uint64_t>::max() - now) {
    SetError({base::ErrorCode::kResourceExhausted, {}}, "trading_drain",
             "shutdown deadline overflowed");
    drain_viable_ = false;
  } else {
    shutdown_deadline_ = now + shutdown_timeout_ns_;
  }
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_.Stop();
    SetError(status, "market_data_stop", source_.error_detail());
  }
  if (!result_.ok()) {
    callbacks_enabled_ = false;
  }
}

void StrategyRunner::StopAfterError(bool can_drain) noexcept {
  callbacks_enabled_ = false;
  if (!can_drain) {
    drain_viable_ = false;
  }
  EnterStopping();
}

bool StrategyRunner::IsDrained() const noexcept {
  return trading_service_.is_drained();
}

bool StrategyRunner::FinishStoppingIfReady() noexcept {
  if (state_ != RunnerState::kStopping) {
    return false;
  }
  if (IsDrained()) {
    Finalize();
    return true;
  }
  if (!drain_viable_) {
    Finalize();
    return true;
  }
  if (IsDue(shutdown_deadline_, clock_.Now())) {
    SetError(
        {base::ErrorCode::kIoError, {}}, "trading_drain_timeout",
        "outstanding orders or requests did not drain before the deadline");
    callbacks_enabled_ = false;
    Finalize();
    return true;
  }
  return false;
}

void StrategyRunner::CheckStop() noexcept {
  bool stop = false;
  const auto status = environment_.CheckStop(stop);
  if (!status.ok()) {
    SetError(status, "signal", "could not inspect termination signals");
    StopAfterError(false);
  } else if (stop || stop_requested_) {
    EnterStopping();
  }
}

bool StrategyRunner::DispatchQuote(const model::QuoteTick& tick) noexcept {
  bool accepted = false;
  const auto applied = market_data_.ApplyQuote(tick, accepted);
  if (applied != base::ErrorCode::kOk) {
    SetQuoteError({applied, {}}, "quote",
                  "invalid quote from market-data source", tick);
    StopAfterError(true);
    return false;
  }
  if (!accepted) {
    if (ignored_quote_count_ != std::numeric_limits<std::uint64_t>::max()) {
      ++ignored_quote_count_;
    }
    return true;
  }
  const auto observed = trading_service_.OnQuote(tick);
  if (!observed.ok()) {
    SetQuoteError(observed, "trading_quote", trading_service_.error_detail(),
                  tick);
    StopAfterError(false);
    return false;
  }
  BeginCallback(strategy::CallbackPhase::kTick);
  const auto result = instance_->OnTick(context_, tick);
  EndCallback();
  if (result != base::ErrorCode::kOk) {
    SetQuoteError({result, {}}, "tick", "strategy OnTick returned an error",
                  tick);
    StopAfterError(true);
    return false;
  }
  return true;
}

bool StrategyRunner::DispatchTrading(
    const trading::TradingDispatch& dispatch) noexcept {
  if (!callbacks_enabled_) {
    return true;
  }
  if (dispatch.notify_order) {
    BeginCallback(strategy::CallbackPhase::kOrderUpdate);
    const auto code = instance_->OnOrderUpdate(context_, dispatch.update);
    EndCallback();
    if (code != base::ErrorCode::kOk) {
      SetOrderError({code, {}}, "order_update",
                    "strategy OnOrderUpdate returned an error",
                    dispatch.update.order.client_order_id,
                    dispatch.update.order.report_sequence);
      StopAfterError(true);
      return false;
    }
  }
  if (dispatch.has_fill) {
    BeginCallback(strategy::CallbackPhase::kFill);
    const auto code = instance_->OnFill(context_, dispatch.fill);
    EndCallback();
    if (code != base::ErrorCode::kOk) {
      SetOrderError({code, {}}, "fill", "strategy OnFill returned an error",
                    dispatch.fill.client_order_id,
                    dispatch.update.order.report_sequence);
      StopAfterError(true);
      return false;
    }
  }
  return true;
}

StrategyRunner::QuotePollResult StrategyRunner::PollQuote() noexcept {
  const auto now = clock_.Now();
  model::QuoteTick tick;
  const auto polled = source_.Poll(now, tick);
  if (!polled.status.ok() || polled.state == MarketDataPollState::kError) {
    const auto status = polled.status.ok()
                            ? base::Status{base::ErrorCode::kCorrupt, {}}
                            : polled.status;
    SetError(status, "market_data_poll", source_.error_detail());
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  if (polled.state == MarketDataPollState::kIdle) {
    if (!source_.is_poll_driven() && IsDue(source_.next_deadline(), now)) {
      SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
               "idle source retained an already-due deadline");
      StopAfterError(true);
      return QuotePollResult::kError;
    }
    if (source_.finished()) {
      EnterStopping();
      return QuotePollResult::kIdle;
    }
    // SHM already bounded its scan internally. Retry only in the next Run
    // iteration, so a transport yield does not multiply its scan budget here.
    return QuotePollResult::kIdle;
  }
  if (polled.state != MarketDataPollState::kEvent) {
    SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
             "source returned an unknown polling state");
    StopAfterError(true);
    return QuotePollResult::kError;
  }
  return DispatchQuote(tick) ? QuotePollResult::kEvent
                             : QuotePollResult::kError;
}

bool StrategyRunner::DispatchTimer() noexcept {
  strategy::TimerEvent event;
  if (!timer_.TryTakeDue(clock_.Now(), event)) {
    return true;
  }
  BeginCallback(strategy::CallbackPhase::kTimer);
  const auto code = instance_->OnTimer(context_, event);
  EndCallback();
  if (code != base::ErrorCode::kOk) {
    SetError({code, {}}, "timer", "strategy OnTimer returned an error");
    StopAfterError(true);
    return false;
  }
  return true;
}

bool StrategyRunner::PollTrading() noexcept {
  const auto now = clock_.Now();
  trading::TradingDispatch dispatch;
  bool event = false;
  const auto polled = trading_service_.Poll(now, dispatch, event);
  if (!polled.ok()) {
    SetError(polled, trading_service_.error_stage(),
             trading_service_.error_detail());
    StopAfterError(false);
    return false;
  }
  if (!event) return true;
  return DispatchTrading(dispatch);
}

void StrategyRunner::ProcessBatch() noexcept {
  bool market_idle = false;
  for (std::size_t count = 0; count < kMaxStrategyBatchSize; ++count) {
    const auto now = clock_.Now();
    const bool ready[] = {
        !market_idle && state_ == RunnerState::kRunning && source_started_ &&
            IsDue(source_.next_deadline(), now),
        trading_started_ && trading_service_.has_ready_event(now),
        state_ == RunnerState::kRunning && callbacks_enabled_ &&
            IsDue(timer_.deadline(), now)};
    constexpr std::size_t kEventSourceCount = sizeof(ready) / sizeof(ready[0]);
    std::size_t selected = kEventSourceCount;
    for (std::size_t offset = 0; offset < kEventSourceCount; ++offset) {
      const auto candidate = (next_event_source_ + offset) % kEventSourceCount;
      if (ready[candidate]) {
        selected = candidate;
        break;
      }
    }
    if (selected == kEventSourceCount) {
      return;
    }
    next_event_source_ =
        static_cast<std::uint8_t>((selected + 1) % kEventSourceCount);
    if (selected == 0) {
      const auto polled = PollQuote();
      if (polled == QuotePollResult::kError) {
        return;
      }
      market_idle = polled == QuotePollResult::kIdle;
    } else if (selected == 1) {
      if (!PollTrading()) {
        return;
      }
    } else if (!DispatchTimer()) {
      return;
    }
  }
}

base::Status StrategyRunner::Wait(
    std::optional<std::uint64_t> timeout_ns) noexcept {
  sigset_t wait_mask;
  const int mask_error = pthread_sigmask(SIG_SETMASK, nullptr, &wait_mask);
  if (mask_error != 0) {
    return {base::ErrorCode::kIoError, {mask_error, std::generic_category()}};
  }
  // Preserve unrelated signal blocking. pselect atomically opens the exit
  // signal window with the wait, so a pending stop cannot be lost before sleep.
  if (sigdelset(&wait_mask, SIGINT) != 0 ||
      sigdelset(&wait_mask, SIGTERM) != 0) {
    return {base::ErrorCode::kIoError, {errno, std::generic_category()}};
  }
  struct timespec timeout{};
  if (timeout_ns) {
    // Nanoseconds stored in uint64 fit in the supported 64-bit time_t seconds.
    constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
    timeout.tv_sec = static_cast<time_t>(*timeout_ns / kNanosecondsPerSecond);
    timeout.tv_nsec = static_cast<long>(*timeout_ns % kNanosecondsPerSecond);
  }
  const int result = pselect(0, nullptr, nullptr, nullptr,
                             timeout_ns ? &timeout : nullptr, &wait_mask);
  if (result < 0 && errno != EINTR) {
    return {base::ErrorCode::kIoError, {errno, std::generic_category()}};
  }
  return {};
}

base::Status StrategyRunner::Run() noexcept {
  if (state_ != RunnerState::kRunning || callback_active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  ResetError();
  for (;;) {
    CheckStop();
    if (FinishStoppingIfReady()) {
      return result_;
    }
    if (state_ == RunnerState::kRunning && source_started_) {
      // Discovery/heartbeat work is deliberately outside the hot polling
      // batch. A failed market source must not prevent broker-order draining.
      const auto serviced = source_.Service(clock_.Now());
      if (!serviced.ok()) {
        SetError(serviced, "market_data_service", source_.error_detail());
        StopAfterError(true);
      }
    }
    if (state_ == RunnerState::kRunning && source_started_ &&
        source_.finished()) {
      EnterStopping();
    }
    if (trading_started_ && drain_viable_) {
      const auto sent = trading_service_.ProcessRequests(clock_.Now());
      if (!sent.ok()) {
        SetError(sent, "trading_send", trading_service_.error_detail());
        StopAfterError(false);
      }
    }
    if (FinishStoppingIfReady()) {
      return result_;
    }
    ProcessBatch();
    CheckStop();
    if (FinishStoppingIfReady()) {
      return result_;
    }
    const auto now = clock_.Now();
    const auto deadline = next_deadline();
    if (IsDue(deadline, now)) {
      continue;
    }
    const auto timeout =
        deadline ? std::optional<std::uint64_t>{*deadline - now} : std::nullopt;
    const auto waited = Wait(timeout);
    if (!waited.ok()) {
      SetError(waited, "wait", "could not wait for data or termination");
      StopAfterError(false);
    }
  }
}

void StrategyRunner::ReportUnconfirmed() const noexcept {
  bool reported_cash = false;
  for (std::size_t index = 0; index < trading_service_.order_count(); ++index) {
    model::OrderSnapshot order;
    if (!trading_service_.TryGetOrder({index + 1}, order)) {
      continue;
    }
    const bool terminal = order.state == model::OrderState::kFilled ||
                          order.state == model::OrderState::kCanceled ||
                          order.state == model::OrderState::kRejected;
    if (terminal && !order.cancel_pending) {
      continue;
    }
    if (!reported_cash) {
      model::CashSnapshot cash;
      if (trading_service_.TryGetCash(cash)) {
        std::fprintf(stderr,
                     "yquant-strategyd: cash_balance=%lld reserved=%lld "
                     "available=%lld unit=micro_yuan\n",
                     static_cast<long long>(cash.balance),
                     static_cast<long long>(cash.reserved),
                     static_cast<long long>(cash.available));
      }
      model::FuturesAccountSnapshot futures;
      if (trading_service_.TryGetFuturesAccount(futures)) {
        std::fprintf(
            stderr,
            "yquant-strategyd: futures_balance=%lld equity=%lld available=%lld "
            "margin=%lld reserved=%lld realized_pnl=%lld unrealized_pnl=%lld "
            "commission=%lld unit=micro_yuan\n",
            static_cast<long long>(futures.balance),
            static_cast<long long>(futures.equity),
            static_cast<long long>(futures.available),
            static_cast<long long>(futures.margin),
            static_cast<long long>(futures.reserved),
            static_cast<long long>(futures.realized_pnl),
            static_cast<long long>(futures.unrealized_pnl),
            static_cast<long long>(futures.commission));
      }
      reported_cash = true;
    }
    std::fprintf(stderr,
                 "yquant-strategyd: unconfirmed_order=%llu state=%u "
                 "instrument_id=%llu side=%u price=%lld quantity=%lld "
                 "filled=%lld leaves=%lld cancel_pending=%u "
                 "cancel_request=%llu effect=%u\n",
                 static_cast<unsigned long long>(order.client_order_id.value),
                 static_cast<unsigned>(order.state),
                 static_cast<unsigned long long>(order.instrument_id.value),
                 static_cast<unsigned>(order.side),
                 static_cast<long long>(order.limit_price),
                 static_cast<long long>(order.original_qty),
                 static_cast<long long>(order.cumulative_filled_qty),
                 static_cast<long long>(order.leaves_qty),
                 static_cast<unsigned>(order.cancel_pending),
                 static_cast<unsigned long long>(order.cancel_request_id.value),
                 static_cast<unsigned>(order.position_effect));
    model::PositionSnapshot position;
    if (trading_service_.TryGetPosition(order.instrument_id, position)) {
      std::fprintf(stderr,
                   "yquant-strategyd: instrument_id=%llu total_qty=%lld "
                   "available_qty=%lld reserved_qty=%lld\n",
                   static_cast<unsigned long long>(order.instrument_id.value),
                   static_cast<long long>(position.total_qty),
                   static_cast<long long>(position.available_qty),
                   static_cast<long long>(position.reserved_qty));
    }
    model::FuturesPositionSnapshot futures;
    if (trading_service_.TryGetFuturesPosition(order.instrument_id, futures)) {
      std::fprintf(stderr,
                   "yquant-strategyd: instrument_id=%llu long_today=%lld/%lld "
                   "long_yesterday=%lld/%lld "
                   "short_today=%lld/%lld short_yesterday=%lld/%lld "
                   "unit=quantity/frozen_lots\n",
                   static_cast<unsigned long long>(order.instrument_id.value),
                   static_cast<long long>(futures.long_today.quantity),
                   static_cast<long long>(futures.long_today.frozen_qty),
                   static_cast<long long>(futures.long_yesterday.quantity),
                   static_cast<long long>(futures.long_yesterday.frozen_qty),
                   static_cast<long long>(futures.short_today.quantity),
                   static_cast<long long>(futures.short_today.frozen_qty),
                   static_cast<long long>(futures.short_yesterday.quantity),
                   static_cast<long long>(futures.short_yesterday.frozen_qty));
    }
  }
}

void StrategyRunner::Finalize() noexcept {
  if (state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  trading_service_.SetAcceptingRequests(false);
  timer_.BeginStopping();
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_.Stop();
    SetError(status, "market_data_stop", source_.error_detail());
  }
  if (started_) {
    started_ = false;
    BeginCallback(strategy::CallbackPhase::kStop);
    const auto stopped = instance_->OnStop(context_);
    EndCallback();
    SetError({stopped, {}}, "stop", "strategy OnStop returned an error");
  }
  if (instance_ != nullptr) {
    library_.plugin()->destroy(instance_);
    instance_ = nullptr;
  }
  const auto closed = library_.Close();
  SetError(closed, library_.error_stage(), library_.error_detail());
  if (!IsDrained()) {
    ReportUnconfirmed();
  }
  trading_started_ = false;
  if (trading_prepared_) {
    trading_prepared_ = false;
    const auto stopped = trading_service_.Stop();
    SetError(stopped, "trading_stop", trading_service_.error_detail());
  }
  trading_service_.Reset();
  timer_.Reset();
  market_data_.Reset();
  state_ = RunnerState::kFinished;
}

}  // namespace yquant::strategyd
