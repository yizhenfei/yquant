#include "yquant/market/sources.h"

#include <stdint.h>

#include <cstdio>
#include <limits>

#include "yquant/base/format.h"
#include "yquant/market/quote_codec.h"

namespace yquant::market {
namespace {

constexpr uint64_t kControlIntervalNs = 250000000;

// Saturates instead of wrapping a monotonic control deadline.
uint64_t NextControl(uint64_t now) noexcept {
  return now > std::numeric_limits<uint64_t>::max() - kControlIntervalNs
             ? std::numeric_limits<uint64_t>::max()
             : now + kControlIntervalNs;
}

}  // namespace

base::Status MockSource::Init(const market::MockQuoteConfig& config,
                              uint64_t now) noexcept {
  auto status = source_.Prepare(config);
  if (!status.ok()) {
    return status;
  }
  return source_.Start(now);
}

PollResult MockSource::Poll(uint64_t now, model::QuoteTick& out) noexcept {
  const auto result = source_.Poll(now, out);
  switch (result.state) {
    case market::MockQuotePollState::kEvent:
      return {PollState::kEvent, result.status};
    case market::MockQuotePollState::kIdle:
      return {PollState::kIdle, result.status};
    case market::MockQuotePollState::kError:
      return {PollState::kError, result.status};
  }
  return {PollState::kError, {base::ErrorCode::kInternalError, {}}};
}

void MockSource::Close() noexcept { (void)source_.Stop(); }

const char* MockSource::error_detail() const noexcept {
  return source_.error_detail();
}

base::Status ShmSource::Init(const market::Endpoint& endpoint) noexcept {
  if (active_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  const auto status = reader_.Open(endpoint);
  if (!status.ok()) {
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_),
                             "cannot open shared-memory input");
    return status;
  }
  if (reader_.max_payload_size() < market::kQuoteFrameSize) {
    reader_.Close();
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_),
                             "input payload limit is smaller than quote frame");
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  active_ = true;
  failure_ = {};
  error_detail_[0] = '\0';
  next_refresh_ = 0;
  refreshed_ = {};
  return {};
}

PollResult ShmSource::Fail(base::Status status, const char* detail) noexcept {
  if (failure_.ok()) {
    failure_ = status;
    ::yquant::base::FormatTo(error_detail_, sizeof(error_detail_), "{}",
                             detail);
  }
  return {PollState::kError, failure_};
}

PollResult ShmSource::Poll(uint64_t now, model::QuoteTick& out) noexcept {
  if (!failure_.ok()) {
    return {PollState::kError, failure_};
  }
  if (!active_) {
    return Fail({base::ErrorCode::kInvalidState, {}}, "input is closed");
  }
  if (now >= next_refresh_) {
    const auto status = reader_.Refresh(refreshed_);
    if (!status.ok()) {
      return Fail(status, "input discovery refresh failed");
    }
    next_refresh_ = NextControl(now);
    if (refreshed_.status == market::RefreshStatus::kStreamChanged) {
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input stream generation changed");
    }
  }
  const auto result = reader_.TryNext(buffer_);
  switch (result.status) {
    case market::ReadStatus::kEvent: {
      model::QuoteTick decoded;
      const auto status = market::DecodeQuoteEvent(buffer_.view(), decoded);
      if (!status.ok()) {
        return Fail(status, "input quote frame is corrupt");
      }
      decoded.receive_time_ns = now;
      out = decoded;
      return {PollState::kEvent, {}};
    }
    case market::ReadStatus::kEmpty:
    case market::ReadStatus::kRetry:
      // A vanished/stalled writer can leave valid committed quotes. Drain
      // those first; never invent a successful recovery after the backlog.
      if (refreshed_.health == market::PublisherHealth::kStale) {
        return Fail({base::ErrorCode::kIoError, {}},
                    "input publisher heartbeat is stale");
      }
      if (refreshed_.status == market::RefreshStatus::kMissing) {
        return Fail({base::ErrorCode::kIoError, {}},
                    "input discovery is missing");
      }
      return {};
    case market::ReadStatus::kGap: {
      char detail[256];
      if (result.gap.has_value()) {
        ::yquant::base::FormatTo(detail, sizeof(detail),
                                 "input gap: expected={} resume_candidate={}",
                                 result.gap->expected_sequence,
                                 result.gap->resume_candidate);
      } else {
        ::yquant::base::FormatTo(detail, sizeof(detail), "input gap");
      }
      return Fail({base::ErrorCode::kCorrupt, {}}, detail);
    }
    case market::ReadStatus::kStreamChanged:
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input stream generation changed");
    case market::ReadStatus::kEndOfGeneration:
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input generation ended");
    case market::ReadStatus::kClosed:
      return Fail({base::ErrorCode::kInvalidState, {}}, "input is closed");
    case market::ReadStatus::kError:
      return Fail(result.error.ok()
                      ? base::Status{base::ErrorCode::kCorrupt, {}}
                      : result.error,
                  "input stream is corrupt");
  }
  return Fail({base::ErrorCode::kInternalError, {}}, "unexpected input result");
}

void ShmSource::Close() noexcept {
  reader_.Close();
  active_ = false;
}

}  // namespace yquant::market
