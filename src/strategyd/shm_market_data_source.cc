#include "strategyd/shm_market_data_source.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>

#include "yquant/base/format.h"
#include "yquant/md/event_filter.h"
#include "yquant/md/quote_codec.h"

namespace yquant::strategyd {
namespace {

// Adds a scheduling interval, saturating instead of wrapping into the past.
uint64_t After(uint64_t now, uint64_t interval) noexcept {
  return now > std::numeric_limits<uint64_t>::max() - interval
             ? std::numeric_limits<uint64_t>::max()
             : now + interval;
}

}  // namespace

base::Status ShmMarketDataSource::Error(base::Status status,
                                        const char* detail) noexcept {
  ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}", detail);
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
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}: {}",
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
    std::span<const model::InstrumentId> subscriptions, uint64_t now) noexcept {
  if (state_ != State::kPrepared) {
    return Error({base::ErrorCode::kInvalidState, {}},
                 "source must be prepared before activation");
  }
  if (subscriptions.size() > md::kMaxMarketDataInstruments) {
    return Error({base::ErrorCode::kInvalidArgument, {}},
                 "too many market data subscriptions");
  }
  std::array<uint64_t, md::kMaxMarketDataInstruments> instruments{};
  size_t instrument_count = 0;
  std::array<bool, md::kMaxMarketDataInstruments> selected = {};
  for (const auto id : subscriptions) {
    size_t index = 0;
    while (index < config_.instrument_count &&
           config_.instruments[index].instrument.id != id) {
      ++index;
    }
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
    ::yquant::base::FormatTo(
        detail, sizeof(detail),
        "could not attach shm stream={} runtime_directory={}", config_.stream,
        ::yquant::base::CStringPrefix(config_.runtime_directory, 500));
    return Error(status, detail);
  }
  instruments_ = instruments;
  instrument_count_ = instrument_count;
  state_ = State::kActive;
  deadline_ =
      subscriptions.empty() ? std::nullopt : std::optional<uint64_t>{now};
  service_deadline_ = After(now, kShmHealthCheckIntervalNs);
  health_ = {};
  health_.health = md::PublisherHealth::kReady;
  error_detail_[0] = '\0';
  return {};
}

base::Status ShmMarketDataSource::Service(uint64_t now) noexcept {
  if (state_ == State::kFailed) {
    return terminal_status_;
  }
  if (state_ != State::kActive || !service_deadline_ ||
      now < *service_deadline_) {
    return {};
  }
  md::RefreshResult refreshed;
  const auto status = reader_.Refresh(refreshed);
  if (!status.ok()) {
    return PollError(status, "could not refresh shm publisher discovery")
        .status;
  }
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
    // Poll can still deliver retained quotes; it reports cached health failure
    // only when a read yields no event. Maintenance must not consume events.
    deadline_ = now;
  }
  return {};
}

MarketDataPollResult ShmMarketDataSource::Poll(uint64_t now,
                                               model::QuoteTick& out) noexcept {
  if (state_ == State::kFailed) {
    return {MarketDataPollState::kError, terminal_status_};
  }
  if (state_ != State::kActive) {
    return {MarketDataPollState::kError,
            Error({base::ErrorCode::kInvalidState, {}},
                  "source must be active when polling")};
  }
  if (!deadline_ || now < *deadline_) {
    return {};
  }
  // Bound filtering independently of reader retries so unrelated events cannot
  // keep the host from checking stop requests and servicing other sources.
  constexpr size_t kMaxEventsPerPoll = 64;
  constexpr uint32_t kEventTypes[] = {md::kQuoteEventType};
  const md::EventFilter filter{std::span(instruments_).first(instrument_count_),
                               kEventTypes, false, false};
  size_t attempts_left = md::ReadBudget{}.max_attempts;
  for (size_t scanned = 0; scanned < kMaxEventsPerPoll && attempts_left != 0;
       ++scanned) {
    const auto result = reader_.TryNext(buffer_, {attempts_left});
    attempts_left -= result.attempts;
    switch (result.status) {
      case md::ReadStatus::kEvent: {
        if (!filter.Matches(buffer_.header)) {
          continue;
        }
        model::QuoteTick quote;
        const auto status = md::DecodeQuoteEvent(buffer_.view(), quote);
        if (!status.ok()) {
          return PollError(status, "invalid shm quote envelope or frame");
        }
        if (quote.source_id != config_.source_id) {
          char detail[128];
          ::yquant::base::FormatTo(
              detail, sizeof(detail),
              "shm quote source_id={} does not match configured {}",
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
          ::yquant::base::FormatTo(
              detail, sizeof(detail),
              "shm market-data gap: expected_sequence={} resume_candidate={}",
              result.gap->expected_sequence, result.gap->resume_candidate);
        } else {
          ::yquant::base::FormatTo(detail, sizeof(detail),
                                   "shm market-data gap");
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
  // Stay ready after exhausting either budget, but let the host end this
  // source's batch before retrying so an active stream cannot monopolize it.
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
