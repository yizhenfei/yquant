#include "strategyd/shm_market_data_source.h"

#include <array>
#include <cstdio>
#include <limits>

#include "yquant/md/event_filter.h"
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
    const md::MarketDataConfig& config) noexcept {
  if (state_ != State::kUnprepared) {
    return Error({base::ErrorCode::kInvalidState, {}},
                 "source can only be prepared once before activation");
  }
  base::ConfigError error;
  const auto result = md::ValidateMarketDataConfig(config, error);
  if (!result.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return result;
  }
  if (config.adapter != md::MarketDataAdapter::kShm) {
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
  if (subscriptions.size() > md::kMaxMarketDataInstruments) {
    return Error({base::ErrorCode::kInvalidArgument, {}},
                 "too many market data subscriptions");
  }
  std::array<std::uint64_t, md::kMaxMarketDataInstruments> instruments{};
  std::size_t instrument_count = 0;
  std::array<bool, md::kMaxMarketDataInstruments> selected = {};
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
    instruments[instrument_count++] = id.value;
  }
  const md::Endpoint endpoint{config_.runtime_directory, config_.stream};
  const auto status = reader_.Open(endpoint, md::StartPosition::Live());
  if (!status.ok()) {
    char detail[sizeof(error_detail_)];
    std::snprintf(detail, sizeof(detail),
                  "could not attach shm stream=%s runtime_directory=%.*s",
                  config_.stream, 500, config_.runtime_directory);
    return Error(status, detail);
  }
  instruments_ = instruments;
  instrument_count_ = instrument_count;
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
  const auto status = reader_.Refresh(refreshed);
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
  // Bound receiver-side filtering so a busy unrelated stream yields to the
  // host's stop check. The broadcast reader itself returns every event.
  constexpr std::size_t kMaxEventsPerPoll = 64;
  constexpr std::uint32_t kEventTypes[] = {md::kQuoteEventType};
  const md::EventFilter filter{std::span(instruments_).first(instrument_count_),
                               kEventTypes, false, false};
  std::size_t attempts_left = md::ReadBudget{}.max_attempts;
  for (std::size_t scanned = 0;
       scanned < kMaxEventsPerPoll && attempts_left != 0; ++scanned) {
    const auto result = reader_.TryNext(buffer_, {attempts_left});
    attempts_left -= result.attempts;
    switch (result.status) {
      case md::ReadStatus::kEvent: {
        if (!filter.Matches(buffer_.header)) continue;
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
      case md::ReadStatus::kGap: {
        char detail[256];
        if (result.gap) {
          std::snprintf(
              detail, sizeof(detail),
              "shm market-data gap: expected_sequence=%llu "
              "resume_candidate=%llu",
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
                         "shm reader unexpectedly closed");
      case md::ReadStatus::kError:
        return PollError(result.error.ok()
                             ? base::Status{base::ErrorCode::kCorrupt, {}}
                             : result.error,
                         "shm reader read failed");
    }
    return PollError({base::ErrorCode::kCorrupt, {}},
                     "shm reader returned an unknown read status");
  }
  // Exhausted receiver scan/attempt budget: yield to the host before retrying.
  deadline_ = now;
  return {};
}

base::Status ShmMarketDataSource::Stop() noexcept {
  reader_.Close();
  deadline_.reset();
  service_deadline_.reset();
  state_ = State::kStopped;
  return {};
}

}  // namespace yquant::strategyd
