#include "marketd/replay_source.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <utility>

#include "marketd/csv_replay.h"
#include "marketd/recording.h"

namespace yquant::marketd {

base::Status ReplaySource::Init(const ReplayConfig& config) noexcept {
  Close();
  failure_ = {};
  error_detail_[0] = '\0';
  if (config.path[0] == '\0' ||
      std::memchr(config.path, '\0', sizeof(config.path)) == nullptr) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "replay path is empty or too long")
        .status;
  }
  if (!std::isfinite(config.speed) || config.speed < 0 ||
      config.max_records == 0 || config.max_records > kMaxReplayRecords) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "invalid replay speed or record limit")
        .status;
  }

  ReplayData loaded;
  base::Status status;
  switch (config.format) {
    case ReplayFormat::kRecording:
      status = LoadRecordingFile(config.path, config.max_records, loaded,
                                 std::span(error_detail_));
      break;
    case ReplayFormat::kDolphinDbCsv:
      status = LoadDolphinDbCsv(config, loaded, std::span(error_detail_));
      break;
    default:
      return Fail({base::ErrorCode::kInvalidArgument, {}},
                  "unknown replay format")
          .status;
  }
  if (!status.ok()) {
    failure_ = status;
    if (error_detail_[0] == '\0')
      std::snprintf(error_detail_, sizeof(error_detail_),
                    "cannot load replay input");
    return status;
  }

  // Comparing to 2^64 remains safe when long double has only double precision;
  // converting UINT64_MAX to that type may round up to the same boundary.
  const long double limit =
      std::ldexp(1.0L, std::numeric_limits<std::uint64_t>::digits);
  for (std::size_t i = 0; i < loaded.size(); ++i) {
    if (config.speed == 0) {
      loaded[i].offset_ns = 0;
    } else if (config.speed != 1) {
      const long double scaled =
          static_cast<long double>(loaded[i].offset_ns) / config.speed;
      if (!std::isfinite(scaled) || scaled >= limit)
        return Fail({base::ErrorCode::kInvalidArgument, {}},
                    "scaled replay duration exceeds the clock range")
            .status;
      loaded[i].offset_ns = static_cast<std::uint64_t>(scaled);
    }
  }
  data_ = std::move(loaded);
  start_delay_ = config.start_delay_ns;
  active_ = true;
  return {};
}

PollResult ReplaySource::Fail(base::Status status,
                              const char* detail) noexcept {
  if (failure_.ok()) {
    failure_ = status;
    std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  }
  return {PollState::kError, failure_};
}

PollResult ReplaySource::Poll(std::uint64_t now,
                              model::QuoteTick& out) noexcept {
  if (!failure_.ok()) return {PollState::kError, failure_};
  if (!active_)
    return Fail({base::ErrorCode::kInvalidState, {}}, "replay input is closed");
  if (index_ == data_.size()) return {PollState::kEnd, {}};
  if (!started_) {
    start_time_ = now;
    last_time_ = now;
    started_ = true;
  } else if (now < last_time_) {
    return Fail({base::ErrorCode::kInvalidArgument, {}},
                "replay clock moved backwards");
  }
  last_time_ = now;
  // Separate subtractions avoid overflowing start + delay + offset.
  const auto elapsed = now - start_time_;
  if (elapsed < start_delay_ ||
      elapsed - start_delay_ < data_[index_].offset_ns)
    return {};
  out = data_[index_++].quote;
  out.receive_time_ns = now;
  return {PollState::kEvent, {}};
}

void ReplaySource::Close() noexcept {
  data_.Reset();
  index_ = 0;
  start_time_ = 0;
  last_time_ = 0;
  start_delay_ = 0;
  started_ = false;
  active_ = false;
}

}  // namespace yquant::marketd
