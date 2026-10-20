#include "strategyd/shm_market_data_source.h"

#include <array>
#include <cstdio>
#include <limits>

#include "yquant/md/quote_codec.h"

namespace yquant::strategyd {
namespace {

// Saturating deadlines never wrap a future maintenance/read time into the past.
std::uint64_t After(std::uint64_t now, std::uint64_t interval) noexcept {
  return now > std::numeric_limits<std::uint64_t>::max() - interval
             ? std::numeric_limits<std::uint64_t>::max()
             : now + interval;
}

}  // namespace

base::Status ShmMarketDataSource::Error(base::Status status,
                                        const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return status;
}

MarketDataPollResult ShmMarketDataSource::PollError(
    base::Status status, const char* detail) noexcept {
  terminal_status_ = status;
  state_ = State::kFailed;
  deadline_.reset();
  service_deadline_.reset();
  return {MarketDataPollState::kError, Error(status, detail)};
}

base::Status ShmMarketDataSource::Prepare(
    const MarketDataConfig& config) noexcept {
  if (state_ != State::kUnprepared) {
    return Error({base::ErrorCode::kInvalidState, {}},
                 "source can only be prepared once before activation");
  }
  strategy::ConfigError error;
  const auto result = ValidateMarketDataConfig(config, error);
  if (!result.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return result;
  }
  if (config.adapter != MarketDataAdapter::kShm) {
    return Error({base::ErrorCode::kInvalidArgument, {}},
                 "shm source requires adapter=shm");
  }
  config_ = config;
  state_ = State::kPrepared;
  error_detail_[0] = '\0';
  return {};
}

base::Status ShmMarketDataSource::Start(
    std::span<const model::InstrumentId> subscriptions,
    std::uint64_t now) noexcept {
  if (state_ != State::kPrepared) {
    return Error({base::ErrorCode::kInvalidState, {}},
                 "source must be prepared before activation");
  }
  if (subscriptions.size() > kMaxMarketDataInstruments) {
    return Error({base::ErrorCode::kInvalidArgument, {}},
                 "too many market data subscriptions");
  }
  md::Subscription filter;
  filter.all_instruments = false;
  filter.all_event_types = false;
  filter.event_type_count = 1;
  filter.event_types[0] = md::kQuoteEventType;
  std::array<bool, kMaxMarketDataInstruments> selected = {};
  for (const auto id : subscriptions) {
    std::size_t index = 0;
    while (index < config_.instrument_count &&
           config_.instruments[index].instrument.id != id)
      ++index;
    if (index == config_.instrument_count || selected[index]) {
      return Error({base::ErrorCode::kInvalidArgument, {}},
                   "subscription contains an unknown or duplicate instrument");
    }
    selected[index] = true;
    filter.instruments[filter.instrument_count++] = id.value;
  }
  const md::Endpoint endpoint{config_.runtime_directory, config_.stream};
  const auto status =
      subscriber_.Open(endpoint, filter, md::StartPosition::Live());
  if (!status.ok()) {
    char detail[sizeof(error_detail_)];
    std::snprintf(detail, sizeof(detail),
                  "could not attach shm stream=%s runtime_directory=%.*s",
                  config_.stream, 500, config_.runtime_directory);
    return Error(status, detail);
  }
  state_ = State::kActive;
  deadline_ =
      subscriptions.empty() ? std::nullopt : std::optional<std::uint64_t>{now};
  service_deadline_ = After(now, kShmHealthCheckIntervalNs);
  health_ = {};
  health_.health = md::PublisherHealth::kReady;
  error_detail_[0] = '\0';
  return {};
}

base::Status ShmMarketDataSource::Service(std::uint64_t now) noexcept {
  if (state_ == State::kFailed) return terminal_status_;
  if (state_ != State::kActive || !service_deadline_ ||
      now < *service_deadline_)
    return {};
  md::RefreshResult refreshed;
  const auto status = subscriber_.Refresh(refreshed);
  if (!status.ok())
    return PollError(status, "could not refresh shm publisher discovery")
        .status;
  health_ = refreshed;
  if (health_.status == md::RefreshStatus::kStreamChanged) {
    return PollError({base::ErrorCode::kInvalidState, {}},
                     "shm stream generation changed; strategy restart required")
        .status;
  }
  service_deadline_ = After(now, kShmHealthCheckIntervalNs);
  if (health_.status == md::RefreshStatus::kMissing ||
      health_.health == md::PublisherHealth::kStale ||
      health_.health == md::PublisherHealth::kStopped) {
    // Poll remains responsible for delivery order: retain queued events and
    // only report cached publisher failure after the retained data is drained.
    deadline_ = now;
  }
  return {};
}

MarketDataPollResult ShmMarketDataSource::Poll(std::uint64_t now,
                                               model::QuoteTick& out) noexcept {
  if (state_ == State::kFailed)
    return {MarketDataPollState::kError, terminal_status_};
  if (state_ != State::kActive) {
    return {MarketDataPollState::kError,
            Error({base::ErrorCode::kInvalidState, {}},
                  "source must be active when polling")};
  }
  if (!deadline_ || now < *deadline_) return {};
  const auto result = subscriber_.TryNext(buffer_);
  switch (result.status) {
    case md::ReadStatus::kEvent: {
      model::QuoteTick quote;
      const auto status = md::DecodeQuoteEvent(buffer_.view(), quote);
      if (!status.ok()) {
        return PollError(status, "invalid shm quote envelope or frame");
      }
      if (quote.source_id != config_.source_id) {
        char detail[128];
        std::snprintf(detail, sizeof(detail),
                      "shm quote source_id=%u does not match configured %u",
                      quote.source_id.value, config_.source_id.value);
        return PollError({base::ErrorCode::kCorrupt, {}}, detail);
      }
      quote.receive_time_ns = now;
      deadline_ = now;
      out = quote;
      return {MarketDataPollState::kEvent, {}};
    }
    case md::ReadStatus::kEmpty:
    case md::ReadStatus::kRetry:
      if (health_.status == md::RefreshStatus::kMissing) {
        return PollError({base::ErrorCode::kInvalidState, {}},
                         "shm publisher discovery disappeared");
      }
      if (health_.health == md::PublisherHealth::kStale) {
        return PollError(
            {base::ErrorCode::kInvalidState, {}},
            "shm publisher heartbeat is stale for more than 3 seconds");
      }
      if (health_.health == md::PublisherHealth::kStopped) {
        return PollError({base::ErrorCode::kInvalidState, {}},
                         "shm publisher stopped; strategy restart required");
      }
      deadline_ = result.status == md::ReadStatus::kEmpty
                      ? After(now, config_.poll_interval_ns)
                      : now;
      return {};
    case md::ReadStatus::kYield:
      // There may be buffered work: return to the host's stop check, then retry
      // without imposing the configured empty-stream wait.
      deadline_ = now;
      return {};
    case md::ReadStatus::kGap: {
      char detail[256];
      if (result.gap) {
        std::snprintf(
            detail, sizeof(detail),
            "shm market-data gap: expected_sequence=%llu resume_candidate=%llu",
            static_cast<unsigned long long>(result.gap->expected_sequence),
            static_cast<unsigned long long>(result.gap->resume_candidate));
      } else {
        std::snprintf(detail, sizeof(detail), "shm market-data gap");
      }
      return PollError({base::ErrorCode::kCorrupt, {}}, detail);
    }
    case md::ReadStatus::kStreamChanged:
      return PollError(
          {base::ErrorCode::kInvalidState, {}},
          "shm stream generation changed; strategy restart required");
    case md::ReadStatus::kEndOfGeneration:
      return PollError({base::ErrorCode::kInvalidState, {}},
                       "shm publisher stopped; strategy restart required");
    case md::ReadStatus::kClosed:
      return PollError({base::ErrorCode::kInvalidState, {}},
                       "shm subscriber unexpectedly closed");
    case md::ReadStatus::kError:
      return PollError(result.error.ok()
                           ? base::Status{base::ErrorCode::kCorrupt, {}}
                           : result.error,
                       "shm subscriber read failed");
  }
  return PollError({base::ErrorCode::kCorrupt, {}},
                   "shm subscriber returned an unknown read status");
}

base::Status ShmMarketDataSource::Stop() noexcept {
  subscriber_.Close();
  deadline_.reset();
  service_deadline_.reset();
  state_ = State::kStopped;
  return {};
}

}  // namespace yquant::strategyd
