#include "strategyd/strategy_runner.h"

#include <cstdio>
#include <limits>

#include "strategyd/strategy_config_loader.h"

namespace yquant::strategyd {

StrategyRunner::StrategyRunner(const MonotonicClock* clock,
                               MarketDataSource* source,
                               EventLoopEnvironment* environment) noexcept
    : clock_(clock != nullptr ? *clock : default_clock_),
      market_data_(clock_),
      context_(clock_, market_data_),
      source_(source != nullptr ? *source : default_source_),
      startup_environment_(environment) {}

StrategyRunner::~StrategyRunner() noexcept {
  // Explicit Stop reports errors; this is the ownership safety net.
  Cleanup();
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

void StrategyRunner::SetConfigError(
    base::Status status, const char* stage,
    const strategy::ConfigError& error) noexcept {
  char detail[kErrorDetailSize];
  std::snprintf(detail, sizeof(detail), "%s: %s%s", error.field_path,
                error.message, error.truncated ? " [truncated]" : "");
  SetError(status, stage, detail);
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
  if (startup_environment_ == nullptr) {
    return false;
  }
  bool stop = false;
  const auto status = startup_environment_->CheckStop(stop);
  if (!status.ok()) {
    SetError(status, "signal", "could not inspect termination signals");
  }
  if (stop || !result_.ok()) {
    Cleanup();
    return true;
  }
  return false;
}

base::Status StrategyRunner::Handle(LoadAndStart command) noexcept {
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
  StrategyConfigLoader config_loader;
  const base::Status config_result = config_loader.Load(command.config_path);
  if (!config_result.ok()) {
    SetConfigError(config_result, "config", config_loader.error());
    Cleanup();
    return result_;
  }
  strategy_id_ = config_loader.strategy_id();
  if (CancelStartupIfRequested()) {
    return result_;
  }

  if (command.market_data_config_path != nullptr) {
    MarketDataConfigLoader market_loader;
    const auto status = market_loader.Load(command.market_data_config_path);
    if (!status.ok()) {
      SetConfigError(status, "market_data_config", market_loader.error());
      Cleanup();
      return result_;
    }
    // Prepare may acquire resources before reporting failure; Stop is safe
    // even when preparation did not finish.
    source_prepared_ = true;
    const auto prepare_status = source_.Prepare(market_loader.config());
    if (!prepare_status.ok()) {
      SetError(prepare_status, "market_data_prepare", source_.error_detail());
      Cleanup();
      return result_;
    }
    const auto market_data_status =
        market_data_.Configure(market_loader.config());
    if (!market_data_status.ok()) {
      SetError(market_data_status, "market_data_prepare",
               "could not prepare the instrument dictionary");
      Cleanup();
      return result_;
    }
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  const base::Status open_result = library_.Open(command.library_path);
  if (!open_result.ok()) {
    SetError(open_result, library_.error_stage(), library_.error_detail());
    Cleanup();
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
    Cleanup();
    return result_;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }

  strategy::ConfigError config_error;
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
    Cleanup();
    return result_;
  }
  if (!config.ready()) {
    SetError({base::ErrorCode::kCorrupt, {}}, "initialize",
             "strategy configuration did not become ready");
    config_loader.Reset();
    Cleanup();
    return result_;
  }
  // JSON views are initialization-only. OnStart must use owned typed fields.
  config_loader.Reset();

  if (CancelStartupIfRequested()) {
    return result_;
  }
  callback_active_ = true;
  market_data_.BeginCallback(CallbackPhase::kStart);
  const base::ErrorCode start_result = instance_->OnStart(context_);
  market_data_.EndCallback();
  callback_active_ = false;
  if (start_result != base::ErrorCode::kOk) {
    SetError({start_result, {}}, "start", "strategy OnStart returned an error");
    Cleanup();
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
      Cleanup();
      return result_;
    }
    source_started_ = true;
  }
  if (CancelStartupIfRequested()) {
    return result_;
  }
  state_ = RunnerState::kRunning;
  return result_;
}

base::Status StrategyRunner::Handle(Stop) noexcept {
  ResetError();
  if (state_ == RunnerState::kStarting || state_ == RunnerState::kStopping ||
      callback_active_) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "a lifecycle callback is already in progress");
    return result_;
  }
  if (state_ != RunnerState::kFinished) {
    Cleanup();
  }
  return result_;
}

std::optional<std::uint64_t> StrategyRunner::next_deadline() const noexcept {
  return state_ == RunnerState::kRunning && source_started_
             ? source_.next_deadline()
             : std::nullopt;
}

base::Status StrategyRunner::DispatchQuote(
    const model::QuoteTick& tick) noexcept {
  ResetError();
  if (state_ != RunnerState::kRunning || callback_active_) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "quotes can only be dispatched in the running state");
    return result_;
  }
  bool accepted = false;
  const auto applied = market_data_.ApplyQuote(tick, accepted);
  if (applied != base::ErrorCode::kOk) {
    SetQuoteError({applied, {}}, "quote",
                  "invalid quote from market-data source", tick);
    Cleanup();
    return result_;
  }
  if (!accepted) {
    if (ignored_quote_count_ != std::numeric_limits<std::uint64_t>::max()) {
      ++ignored_quote_count_;
    }
    return result_;
  }
  callback_active_ = true;
  market_data_.BeginCallback(CallbackPhase::kTick);
  const auto result = instance_->OnTick(context_, tick);
  market_data_.EndCallback();
  callback_active_ = false;
  if (result != base::ErrorCode::kOk) {
    SetQuoteError({result, {}}, "tick", "strategy OnTick returned an error",
                  tick);
    Cleanup();
  }
  return result_;
}

base::Status StrategyRunner::ProcessBatch() noexcept {
  ResetError();
  if (state_ != RunnerState::kRunning || callback_active_) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "market data can only be processed in the running state");
    return result_;
  }
  for (std::size_t count = 0;
       source_started_ && count < kMaxMarketDataBatchSize; ++count) {
    const auto deadline = next_deadline();
    const auto now = clock_.Now();
    if (!deadline || *deadline > now) {
      break;
    }
    model::QuoteTick tick;
    const auto poll = source_.Poll(now, tick);
    if (poll.state == MarketDataPollState::kError || !poll.status.ok()) {
      const auto status = poll.status.ok()
                              ? base::Status{base::ErrorCode::kCorrupt, {}}
                              : poll.status;
      SetError(status, "market_data_poll", source_.error_detail());
      Cleanup();
      return result_;
    }
    if (poll.state == MarketDataPollState::kIdle) {
      const auto next = source_.next_deadline();
      if (next && *next <= clock_.Now()) {
        SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
                 "idle source retained an already-due deadline");
        Cleanup();
      }
      return result_;
    }
    if (poll.state != MarketDataPollState::kEvent) {
      SetError({base::ErrorCode::kCorrupt, {}}, "market_data_poll",
               "source returned an unknown polling state");
      Cleanup();
      return result_;
    }
    const auto status = DispatchQuote(tick);
    if (!status.ok()) {
      return status;
    }
  }
  return result_;
}

base::Status StrategyRunner::Run(EventLoopEnvironment& environment) noexcept {
  ResetError();
  if (state_ != RunnerState::kRunning || callback_active_) {
    SetError({base::ErrorCode::kInvalidState, {}}, "command",
             "event loop requires a running strategy");
    return result_;
  }
  for (;;) {
    bool stop = false;
    const auto check = environment.CheckStop(stop);
    if (!check.ok()) {
      SetError(check, "signal", "could not inspect termination signals");
      Cleanup();
      return result_;
    }
    if (stop) {
      Cleanup();
      return result_;
    }
    const auto now = clock_.Now();
    const auto deadline = next_deadline();
    if (!deadline || *deadline > now) {
      const auto timeout = deadline
                               ? std::optional<std::uint64_t>{*deadline - now}
                               : std::nullopt;
      const auto waited = environment.Wait(timeout);
      if (!waited.ok()) {
        SetError(waited, "wait",
                 "could not wait for market data or termination");
        Cleanup();
        return result_;
      }
      continue;
    }
    const auto processed = ProcessBatch();
    if (!processed.ok()) {
      return processed;
    }
  }
}

void StrategyRunner::Cleanup() noexcept {
  if (state_ == RunnerState::kFinished) {
    return;
  }
  state_ = RunnerState::kStopping;
  source_started_ = false;
  if (source_prepared_) {
    source_prepared_ = false;
    const auto status = source_.Stop();
    SetError(status, "market_data_stop", source_.error_detail());
  }
  if (started_) {
    // Clear first so cleanup never calls OnStop more than once.
    started_ = false;
    callback_active_ = true;
    market_data_.BeginCallback(CallbackPhase::kStop);
    const base::ErrorCode stop_result = instance_->OnStop(context_);
    market_data_.EndCallback();
    callback_active_ = false;
    SetError({stop_result, {}}, "stop", "strategy OnStop returned an error");
  }
  if (instance_ != nullptr) {
    library_.plugin()->destroy(instance_);
    instance_ = nullptr;
  }
  const base::Status close_result = library_.Close();
  SetError(close_result, library_.error_stage(), library_.error_detail());
  market_data_.Reset();
  state_ = RunnerState::kFinished;
}

}  // namespace yquant::strategyd
