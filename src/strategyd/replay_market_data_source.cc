#include "strategyd/replay_market_data_source.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <system_error>
#include <utility>

#include "yquant/md/quote_codec.h"

namespace yquant::strategyd {
namespace {

constexpr std::string_view kCsvHeader =
    "elapsed_ns,event_time_ns,bid_price,bid_qty,ask_price,ask_qty,flags";

// Owns a cold-path descriptor, including every early validation exit.
struct FileDescriptor {
  int value = -1;
  ~FileDescriptor() {
    if (value >= 0) close(value);
  }
};

// The CSV intentionally has no quoting or whitespace coercion. CRLF is
// accepted, and a final line terminator does not create an extra data record.
bool NextLine(std::string_view& remaining, std::string_view& line) noexcept {
  if (remaining.empty()) return false;
  const auto end = remaining.find('\n');
  line = remaining.substr(0, end);
  if (end == remaining.npos)
    remaining = {};
  else
    remaining.remove_prefix(end + 1);
  if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
  return true;
}

// Exact base-10 parsing rejects overflow, spaces, plus signs and fractions.
template <typename Integer>
bool ParseInteger(std::string_view text, Integer& out) noexcept {
  if (text.empty()) return false;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), out);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// Splits exactly seven nonempty unescaped columns without allocation.
bool SplitFields(std::string_view line,
                 std::array<std::string_view, 7>& fields) noexcept {
  for (std::size_t index = 0; index < fields.size(); ++index) {
    const auto comma = line.find(',');
    if ((index + 1 == fields.size()) != (comma == line.npos)) return false;
    fields[index] = line.substr(0, comma);
    if (fields[index].empty()) return false;
    if (comma != line.npos) line.remove_prefix(comma + 1);
  }
  return true;
}

// No file/parser bytes are borrowed by a parsed quote. Metadata adds tick/lot
// constraints to the canonical wire quote validation.
bool ParseRecord(std::string_view line,
                 const model::InstrumentDefinition& definition,
                 std::uint64_t& elapsed_ns, model::QuoteTick& quote) noexcept {
  std::array<std::string_view, 7> fields;
  std::uint32_t flags = 0;
  if (!SplitFields(line, fields) || !ParseInteger(fields[0], elapsed_ns) ||
      !ParseInteger(fields[1], quote.event_time_ns) ||
      !ParseInteger(fields[2], quote.bid_price) ||
      !ParseInteger(fields[3], quote.bid_qty) ||
      !ParseInteger(fields[4], quote.ask_price) ||
      !ParseInteger(fields[5], quote.ask_qty) ||
      !ParseInteger(fields[6], flags) || flags > 255)
    return false;
  quote.flags = static_cast<std::uint8_t>(flags);
  return md::ValidateQuote(quote).ok() &&
         quote.bid_price % definition.price_tick == 0 &&
         quote.ask_price % definition.price_tick == 0 &&
         quote.bid_qty % definition.quantity_step == 0 &&
         quote.ask_qty % definition.quantity_step == 0;
}

}  // namespace

base::Status ReplayMarketDataSource::Error(base::ErrorCode code,
                                           const char* detail) noexcept {
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  return {code, {}};
}

base::Status ReplayMarketDataSource::ReadFrames(
    const md::MarketDataConfig& config) noexcept {
  FileDescriptor file{
      open(config.replay_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)};
  if (file.value < 0) {
    const auto code = errno;
    auto status = Error(base::ErrorCode::kIoError, "could not open replay CSV");
    status.system_error = {code, std::generic_category()};
    return status;
  }
  struct stat info;
  if (fstat(file.value, &info) != 0) {
    const auto code = errno;
    auto status =
        Error(base::ErrorCode::kIoError, "could not inspect replay CSV");
    status.system_error = {code, std::generic_category()};
    return status;
  }
  if (!S_ISREG(info.st_mode))
    return Error(base::ErrorCode::kInvalidArgument,
                 "replay CSV must be a regular file");
  if (info.st_size < 0 ||
      static_cast<std::uint64_t>(info.st_size) > kMaxReplayFileBytes)
    return Error(base::ErrorCode::kResourceExhausted,
                 "replay CSV exceeds 16 MiB");
  std::unique_ptr<char[]> input(
      new (std::nothrow) char[kMaxReplayFileBytes + 1]);
  if (!input)
    return Error(base::ErrorCode::kResourceExhausted,
                 "could not allocate replay input");
  std::size_t bytes = 0;
  while (bytes <= kMaxReplayFileBytes) {
    const auto count =
        read(file.value, input.get() + bytes, kMaxReplayFileBytes + 1 - bytes);
    if (count > 0)
      bytes += static_cast<std::size_t>(count);
    else if (count == 0)
      break;
    else if (errno != EINTR) {
      const auto code = errno;
      auto status =
          Error(base::ErrorCode::kIoError, "could not read replay CSV");
      status.system_error = {code, std::generic_category()};
      return status;
    }
  }
  if (bytes > kMaxReplayFileBytes)
    return Error(base::ErrorCode::kResourceExhausted,
                 "replay CSV exceeds 16 MiB");
  const auto close_result = close(file.value);
  file.value = -1;
  if (close_result != 0) {
    const auto code = errno;
    auto status =
        Error(base::ErrorCode::kIoError, "could not close replay CSV");
    status.system_error = {code, std::generic_category()};
    return status;
  }
  std::string_view remaining(input.get(), bytes);
  std::string_view line;
  if (!NextLine(remaining, line) || line != kCsvHeader)
    return Error(base::ErrorCode::kInvalidArgument,
                 "replay CSV has an invalid header");
  const auto records = remaining;
  std::size_t count = 0;
  while (NextLine(remaining, line)) {
    if (++count > kMaxReplayFrames)
      return Error(base::ErrorCode::kResourceExhausted,
                   "replay CSV exceeds 100000 frames");
  }
  if (count == 0)
    return Error(base::ErrorCode::kInvalidArgument,
                 "replay CSV contains no data frames");
  std::unique_ptr<Frame[]> frames(new (std::nothrow) Frame[count]);
  if (!frames)
    return Error(base::ErrorCode::kResourceExhausted,
                 "could not allocate replay frames");
  remaining = records;
  std::uint64_t previous_elapsed = 0;
  for (std::size_t index = 0; index < count; ++index) {
    NextLine(remaining, line);
    auto& frame = frames[index];
    frame.quote.source_id = config.source_id;
    frame.quote.instrument_id = config.instruments[0].instrument.id;
    frame.quote.sequence = index + 1;
    std::uint64_t elapsed = 0;
    if (!ParseRecord(line, config.instruments[0].instrument, elapsed,
                     frame.quote) ||
        (index != 0 && elapsed < previous_elapsed)) {
      std::snprintf(error_detail_, sizeof(error_detail_),
                    "replay CSV line %zu has invalid quote fields or "
                    "regressing elapsed_ns",
                    index + 2);
      return {base::ErrorCode::kInvalidArgument, {}};
    }
    previous_elapsed = elapsed;
    frame.elapsed_ns = elapsed / config.replay_speed;
  }
  frames_ = std::move(frames);
  frame_count_ = count;
  return {};
}

base::Status ReplayMarketDataSource::Prepare(
    const md::MarketDataConfig& config) noexcept {
  if (state_ != State::kUnprepared)
    return Error(base::ErrorCode::kInvalidState,
                 "source can only be prepared once");
  base::ConfigError error;
  const auto validated = md::ValidateMarketDataConfig(config, error);
  if (!validated.ok()) {
    std::snprintf(error_detail_, sizeof(error_detail_), "%s: %s",
                  error.field_path, error.message);
    return validated;
  }
  if (config.adapter != md::MarketDataAdapter::kReplay)
    return Error(base::ErrorCode::kInvalidArgument,
                 "replay source requires adapter=replay");
  const auto loaded = ReadFrames(config);
  if (!loaded.ok()) return loaded;
  instrument_id_ = config.instruments[0].instrument.id;
  tail_ns_ = config.replay_tail_ns;
  state_ = State::kPrepared;
  error_detail_[0] = '\0';
  return {};
}

base::Status ReplayMarketDataSource::Start(
    std::span<const model::InstrumentId> subscriptions,
    std::uint64_t now) noexcept {
  if (state_ != State::kPrepared)
    return Error(base::ErrorCode::kInvalidState,
                 "source must be prepared before activation");
  if (subscriptions.size() > 1 ||
      (!subscriptions.empty() && subscriptions[0] != instrument_id_))
    return Error(base::ErrorCode::kInvalidArgument,
                 "replay subscription is unknown or duplicated");
  const auto last = frames_[frame_count_ - 1].elapsed_ns;
  constexpr auto kMaxTime = std::numeric_limits<std::uint64_t>::max();
  if (last > kMaxTime - now || tail_ns_ > kMaxTime - now - last)
    return Error(base::ErrorCode::kResourceExhausted,
                 "replay deadline overflows monotonic time");
  start_ns_ = now;
  finish_ns_ = now + last + tail_ns_;
  subscribed_ = !subscriptions.empty();
  next_frame_ = 0;
  finished_ = false;
  deadline_ = subscribed_ ? now + frames_[0].elapsed_ns : finish_ns_;
  state_ = State::kActive;
  error_detail_[0] = '\0';
  return {};
}

MarketDataPollResult ReplayMarketDataSource::Poll(
    std::uint64_t now, model::QuoteTick& out) noexcept {
  if (state_ != State::kActive)
    return {MarketDataPollState::kError,
            Error(base::ErrorCode::kInvalidState,
                  "source must be active when polling")};
  if (!deadline_ || now < *deadline_) return {};
  if (!subscribed_ || next_frame_ == frame_count_) {
    finished_ = true;
    deadline_.reset();
    return {};
  }
  auto quote = frames_[next_frame_].quote;
  quote.receive_time_ns = now;
  ++next_frame_;
  deadline_ = next_frame_ == frame_count_
                  ? finish_ns_
                  : start_ns_ + frames_[next_frame_].elapsed_ns;
  out = quote;
  return {MarketDataPollState::kEvent, {}};
}

base::Status ReplayMarketDataSource::Stop() noexcept {
  deadline_.reset();
  frames_.reset();
  state_ = State::kStopped;
  return {};
}

}  // namespace yquant::strategyd
