#include "marketd/adapters.h"

#include <cstdio>
#include <limits>

#include "yquant/md/quote_codec.h"

namespace yquant::marketd {
namespace {

constexpr std::uint64_t kControlIntervalNs = 250000000;

// Saturates instead of wrapping a monotonic control deadline.
std::uint64_t NextControl(std::uint64_t now) noexcept {
  return now > std::numeric_limits<std::uint64_t>::max() - kControlIntervalNs
             ? std::numeric_limits<std::uint64_t>::max()
             : now + kControlIntervalNs;
}

}  // namespace

base::Status MockSource::Init(const md::MockQuoteConfig& config,
                              std::uint64_t now) noexcept {
  auto status = source_.Prepare(config);
  if (!status.ok()) return status;
  return source_.Start(now);
}

PollResult MockSource::Poll(std::uint64_t now, model::QuoteTick& out) noexcept {
  const auto result = source_.Poll(now, out);
  switch (result.state) {
    case md::MockQuotePollState::kEvent:
      return {PollState::kEvent, result.status};
    case md::MockQuotePollState::kIdle:
      return {PollState::kIdle, result.status};
    case md::MockQuotePollState::kError:
      return {PollState::kError, result.status};
  }
  return {PollState::kError, {base::ErrorCode::kInternalError, {}}};
}

void MockSource::Close() noexcept { (void)source_.Stop(); }

const char* MockSource::error_detail() const noexcept {
  return source_.error_detail();
}

base::Status ShmSource::Init(const md::Endpoint& endpoint) noexcept {
  if (active_) return {base::ErrorCode::kInvalidState, {}};
  const auto status = reader_.Open(endpoint);
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "cannot open shared-memory input");
    return status;
  }
  if (reader_.max_payload_size() < md::kQuoteFrameSize) {
    reader_.Close();
    std::snprintf(error_detail_, sizeof(error_detail_),
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
    std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  }
  return {PollState::kError, failure_};
}

PollResult ShmSource::Poll(std::uint64_t now, model::QuoteTick& out) noexcept {
  if (!failure_.ok()) return {PollState::kError, failure_};
  if (!active_)
    return Fail({base::ErrorCode::kInvalidState, {}}, "input is closed");
  if (now >= next_refresh_) {
    const auto status = reader_.Refresh(refreshed_);
    if (!status.ok()) return Fail(status, "input discovery refresh failed");
    next_refresh_ = NextControl(now);
    if (refreshed_.status == md::RefreshStatus::kStreamChanged)
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input stream generation changed");
  }
  const auto result = reader_.TryNext(buffer_);
  switch (result.status) {
    case md::ReadStatus::kEvent: {
      model::QuoteTick decoded;
      const auto status = md::DecodeQuoteEvent(buffer_.view(), decoded);
      if (!status.ok()) return Fail(status, "input quote frame is corrupt");
      decoded.receive_time_ns = now;
      out = decoded;
      return {PollState::kEvent, {}};
    }
    case md::ReadStatus::kEmpty:
    case md::ReadStatus::kRetry:
      // A vanished/stalled writer can leave valid committed quotes. Drain
      // those first; never invent a successful recovery after the backlog.
      if (refreshed_.health == md::PublisherHealth::kStale)
        return Fail({base::ErrorCode::kIoError, {}},
                    "input publisher heartbeat is stale");
      if (refreshed_.status == md::RefreshStatus::kMissing)
        return Fail({base::ErrorCode::kIoError, {}},
                    "input discovery is missing");
      return {};
    case md::ReadStatus::kGap: {
      char detail[256];
      if (result.gap.has_value()) {
        std::snprintf(
            detail, sizeof(detail),
            "input gap: expected=%llu resume_candidate=%llu",
            static_cast<unsigned long long>(result.gap->expected_sequence),
            static_cast<unsigned long long>(result.gap->resume_candidate));
      } else {
        std::snprintf(detail, sizeof(detail), "input gap");
      }
      return Fail({base::ErrorCode::kCorrupt, {}}, detail);
    }
    case md::ReadStatus::kStreamChanged:
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input stream generation changed");
    case md::ReadStatus::kEndOfGeneration:
      return Fail({base::ErrorCode::kInvalidState, {}},
                  "input generation ended");
    case md::ReadStatus::kClosed:
      return Fail({base::ErrorCode::kInvalidState, {}}, "input is closed");
    case md::ReadStatus::kError:
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

base::Status ShmPublisherAdapter::Init(
    const md::PublishConfig& config) noexcept {
  if (active_) return {base::ErrorCode::kInvalidState, {}};
  if (config.max_payload_size < md::kQuoteFrameSize) {
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "output payload limit is smaller than quote frame");
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const auto status = publisher_.Create(config);
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "cannot create shared-memory output");
    return status;
  }
  active_ = true;
  next_tick_ = 0;
  error_detail_[0] = '\0';
  return {};
}

base::Status ShmPublisherAdapter::Publish(
    const model::QuoteTick& quote) noexcept {
  md::QuoteFrame frame;
  auto status = md::EncodeQuote(quote, frame);
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "invalid quote");
    return status;
  }
  status = publisher_.Publish(md::QuoteEvent(quote, frame));
  if (!status.ok())
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "shared-memory publication failed");
  return status;
}

base::Status ShmPublisherAdapter::Service(std::uint64_t now) noexcept {
  if (!active_) return {base::ErrorCode::kInvalidState, {}};
  if (now < next_tick_) return {};
  const auto status = publisher_.Tick();
  if (!status.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_),
                  "shared-memory heartbeat failed");
    return status;
  }
  next_tick_ = NextControl(now);
  return {};
}

void ShmPublisherAdapter::Close() noexcept {
  publisher_.Close();
  active_ = false;
}

}  // namespace yquant::marketd
