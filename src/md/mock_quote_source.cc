#include "yquant/md/mock_quote_source.h"

#include <chrono>
#include <cstdio>
#include <limits>

namespace yquant::md {

base::Status MockQuoteSource::Error(base::ErrorCode code,
                                    const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

MockQuotePollResult MockQuoteSource::PollError(base::ErrorCode code,
                                               const char* detail) noexcept {
  state_ = State::kFailed;
  deadline_.reset();
  return {MockQuotePollState::kError, Error(code, detail)};
}

base::Status MockQuoteSource::Prepare(const MockQuoteConfig& config) noexcept {
  if (state_ != State::kUnprepared) {
    return Error(base::ErrorCode::kInvalidState,
                 "source can only be prepared once before activation");
  }
  base::ConfigError error;
  const auto result = ValidateMockQuoteConfig(config, error);
  if (!result.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return result;
  }
  config_ = config;
  state_ = State::kPrepared;
  error_detail_[0] = '\0';
  return {};
}

base::Status MockQuoteSource::Start(std::uint64_t now) noexcept {
  if (state_ != State::kPrepared)
    return Error(base::ErrorCode::kInvalidState,
                 "source must be prepared before activation");
  if (now > std::numeric_limits<std::uint64_t>::max() - config_.interval_ns)
    return Error(base::ErrorCode::kResourceExhausted,
                 "initial market data deadline overflows");
  next_index_ = 0;
  shifted_batch_ = false;
  sequences_.fill(0);
  deadline_ = now + config_.interval_ns;
  state_ = State::kActive;
  error_detail_[0] = '\0';
  return {};
}

MockQuotePollResult MockQuoteSource::Poll(std::uint64_t now,
                                          model::QuoteTick& out) noexcept {
  if (state_ != State::kActive) {
    return {MockQuotePollState::kError,
            Error(base::ErrorCode::kInvalidState,
                  "source must be active when polling")};
  }
  if (!deadline_.has_value() || now < *deadline_) return {};
  const auto index = next_index_;
  const auto& item = config_.instruments[index];
  if (sequences_[index] == std::numeric_limits<std::uint64_t>::max()) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "market data sequence exhausted");
  }
  const bool last = next_index_ + 1 == config_.instrument_count;
  if (last &&
      now > std::numeric_limits<std::uint64_t>::max() - config_.interval_ns) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "next market data deadline overflows");
  }
  const auto utc_now = std::chrono::system_clock::now().time_since_epoch();
  // Convert seconds and the fractional part separately: some platforms use a
  // system_clock range wider than signed 64-bit nanoseconds can represent.
  const auto utc_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(utc_now);
  const auto fraction_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               utc_now - utc_seconds)
                               .count();
  if (utc_seconds.count() < 0 || fraction_ns < 0) {
    return PollError(base::ErrorCode::kUnsupported,
                     "system clock precedes the Unix epoch");
  }
  const auto seconds = static_cast<std::uint64_t>(utc_seconds.count());
  const auto fraction = static_cast<std::uint64_t>(fraction_ns);
  constexpr std::uint64_t kNanosecondsPerSecond = 1000000000;
  if (seconds > (UINT64_MAX - fraction) / kNanosecondsPerSecond) {
    return PollError(base::ErrorCode::kResourceExhausted,
                     "system clock exceeds the UTC nanosecond range");
  }
  model::QuoteTick tick;
  tick.source_id = config_.source_id;
  tick.instrument_id = item.instrument.id;
  tick.sequence = sequences_[index] + 1;
  tick.event_time_ns = seconds * kNanosecondsPerSecond + fraction;
  tick.receive_time_ns = now;
  // Prepare validated both sums, so this bounded alternation cannot overflow.
  const auto offset = shifted_batch_ ? item.price_step : 0;
  tick.bid_price = item.bid_price + offset;
  tick.ask_price = item.ask_price + offset;
  tick.bid_qty = item.bid_qty;
  tick.ask_qty = item.ask_qty;
  tick.flags = model::kKnownQuoteFlags;

  // Commit sequence/state only once every possible failure has been checked.
  ++sequences_[index];
  if (last) {
    next_index_ = 0;
    shifted_batch_ = !shifted_batch_;
    deadline_ = now + config_.interval_ns;
  } else {
    ++next_index_;
    deadline_ = now;
  }
  out = tick;
  return {MockQuotePollState::kEvent, {}};
}

base::Status MockQuoteSource::Stop() noexcept {
  state_ = State::kStopped;
  deadline_.reset();
  next_index_ = 0;
  return {};
}

}  // namespace yquant::md
