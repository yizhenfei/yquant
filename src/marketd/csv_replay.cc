#include "marketd/csv_replay.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string_view>
#include <utility>

namespace yquant::marketd {
namespace {

constexpr std::size_t kMaxCsvRecordBytes = 64 * 1024;
constexpr std::size_t kMaxCsvColumns = 256;
constexpr std::size_t kSymbolsPerVenue = 1000000;
constexpr std::uint64_t kNsPerSecond = 1000000000;
constexpr std::array<std::string_view, 8> kRequiredColumns = {
    "Date",     "Time",   "SecurityID", "SecurityIDSource",
    "OfferPX1", "BidPX1", "OfferSize1", "BidSize1"};

// All diagnostics are bounded; a caller may omit the diagnostic buffer.
base::Status Failure(std::span<char> detail, std::size_t line,
                     const char* reason,
                     base::ErrorCode code = base::ErrorCode::kCorrupt,
                     int system_error = 0) noexcept {
  if (!detail.empty()) {
    if (line == 0) {
      std::snprintf(detail.data(), detail.size(), "CSV: %s", reason);
    } else {
      std::snprintf(detail.data(), detail.size(), "CSV line %zu: %s", line,
                    reason);
    }
  }
  return {code, system_error == 0
                    ? std::error_code{}
                    : std::error_code(system_error, std::generic_category())};
}

std::string_view Trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' ||
                            value.front() == '\r'))
    value.remove_prefix(1);
  while (!value.empty() &&
         (value.back() == ' ' || value.back() == '\t' || value.back() == '\r'))
    value.remove_suffix(1);
  return value;
}

// Buffered reads avoid a locked stdio call for every byte. Records may contain
// quoted newlines, but their complete encoded length remains bounded.
class CsvReader {
 public:
  enum class Result { kRecord, kEnd, kTooLong, kInvalid, kIoError };

  explicit CsvReader(std::FILE* file) noexcept : file_(file) {}

  Result Next(std::span<char> output, std::size_t& size,
              std::size_t& first_line) noexcept {
    size = 0;
    first_line = line_;
    bool quoted = false;
    for (;;) {
      if (position_ == available_) {
        available_ = std::fread(buffer_.data(), 1, buffer_.size(), file_);
        position_ = 0;
        if (available_ == 0) {
          if (std::ferror(file_)) return Result::kIoError;
          return size == 0 ? Result::kEnd : Result::kRecord;
        }
      }
      const char value = buffer_[position_++];
      if (value == '\0') return Result::kInvalid;
      if (value == '"') quoted = !quoted;
      if (value == '\n') {
        ++line_;
        if (!quoted) {
          if (size != 0 && output[size - 1] == '\r') --size;
          return Result::kRecord;
        }
      }
      if (size == output.size()) return Result::kTooLong;
      output[size++] = value;
    }
  }

 private:
  std::FILE* file_;
  std::array<char, kMaxCsvRecordBytes> buffer_;
  std::size_t position_ = 0;
  std::size_t available_ = 0;
  std::size_t line_ = 1;
};

// Unescapes fields in place. Each result borrows the supplied record until its
// next modification. Quotes must enclose the entire field; doubled quotes are
// decoded, including in columns ignored by the quote converter.
bool ParseRecord(std::span<char> record,
                 std::array<std::string_view, kMaxCsvColumns>& fields,
                 std::size_t& count) noexcept {
  count = 0;
  std::size_t read = 0;
  std::size_t write = 0;
  for (;;) {
    if (count == fields.size()) return false;
    const std::size_t begin = write;
    if (read < record.size() && record[read] == '"') {
      ++read;
      bool closed = false;
      while (read < record.size()) {
        const char value = record[read++];
        if (value != '"') {
          record[write++] = value;
        } else if (read < record.size() && record[read] == '"') {
          ++read;
          record[write++] = '"';
        } else {
          closed = true;
          break;
        }
      }
      if (!closed || (read != record.size() && record[read] != ','))
        return false;
    } else {
      while (read < record.size() && record[read] != ',') {
        if (record[read] == '"') return false;
        record[write++] = record[read++];
      }
    }
    fields[count++] = {record.data() + begin, write - begin};
    if (read == record.size()) return true;
    ++read;
  }
}

// Parses ASCII digits without signed arithmetic or intermediate overflow.
bool ParseUnsigned(std::string_view value, std::uint64_t limit,
                   std::uint64_t& output) noexcept {
  if (value.empty()) return false;
  std::uint64_t parsed = 0;
  for (const char digit : value) {
    if (digit < '0' || digit > '9') return false;
    const auto number = static_cast<std::uint64_t>(digit - '0');
    if (parsed > limit / 10 || (parsed == limit / 10 && number > limit % 10))
      return false;
    parsed = parsed * 10 + number;
  }
  output = parsed;
  return true;
}

// Uses decimal arithmetic so CSV floating-point export tails cannot perturb the
// configured integer-price representation. Extra digits round half upward.
bool ParsePrice(std::string_view value, std::uint8_t decimals,
                std::int64_t& output) noexcept {
  value = Trim(value);
  const auto point = value.find('.');
  const auto whole = value.substr(0, point);
  std::uint64_t scale = 1;
  for (std::uint8_t i = 0; i < decimals; ++i) scale *= 10;
  constexpr auto kLimit =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  std::uint64_t integer = 0;
  if (!ParseUnsigned(whole, kLimit / scale, integer)) return false;
  std::uint64_t fraction = 0;
  bool round_up = false;
  if (point != std::string_view::npos) {
    const auto fractional = value.substr(point + 1);
    if (fractional.empty()) return false;
    for (std::size_t i = 0; i < fractional.size(); ++i) {
      if (fractional[i] < '0' || fractional[i] > '9') return false;
      if (i < decimals) {
        fraction =
            fraction * 10 + static_cast<std::uint64_t>(fractional[i] - '0');
      } else if (i == decimals) {
        round_up = fractional[i] >= '5';
      }
    }
    for (std::size_t i = fractional.size(); i < decimals; ++i) fraction *= 10;
  }
  const std::uint64_t scaled = integer * scale;
  if (fraction > kLimit - scaled) return false;
  const std::uint64_t result = scaled + fraction;
  if (round_up && result == kLimit) return false;
  output = static_cast<std::int64_t>(result + (round_up ? 1 : 0));
  return true;
}

// A snapshot timestamp is exchange local time (UTC+08), independent of the
// machine's timezone. chrono validates leap years and month lengths.
bool ParseTimestamp(std::string_view date, std::string_view time,
                    std::uint64_t& output) noexcept {
  date = Trim(date);
  time = Trim(time);
  if (date.size() != 10 || date[4] != '.' || date[7] != '.' ||
      time.size() < 8 || time[2] != ':' || time[5] != ':' ||
      (time.size() != 8 &&
       (time.size() < 10 || time.size() > 18 || time[8] != '.')))
    return false;
  std::uint64_t year, month, day, hour, minute, second;
  if (!ParseUnsigned(date.substr(0, 4), 9999, year) || year < 1970 ||
      !ParseUnsigned(date.substr(5, 2), 12, month) || month == 0 ||
      !ParseUnsigned(date.substr(8, 2), 31, day) || day == 0 ||
      !ParseUnsigned(time.substr(0, 2), 23, hour) ||
      !ParseUnsigned(time.substr(3, 2), 59, minute) ||
      !ParseUnsigned(time.substr(6, 2), 59, second))
    return false;
  const std::chrono::year_month_day calendar{
      std::chrono::year(static_cast<int>(year)),
      std::chrono::month(static_cast<unsigned>(month)),
      std::chrono::day(static_cast<unsigned>(day))};
  if (!calendar.ok()) return false;
  std::uint64_t fraction = 0;
  if (time.size() > 8) {
    if (!ParseUnsigned(time.substr(9), kNsPerSecond - 1, fraction))
      return false;
    for (std::size_t i = time.size() - 9; i < 9; ++i) fraction *= 10;
  }
  const std::int64_t seconds =
      std::chrono::sys_days(calendar).time_since_epoch().count() * 86400 +
      static_cast<std::int64_t>(hour * 3600 + minute * 60 + second) - 8 * 3600;
  constexpr auto kLimit = std::numeric_limits<std::uint64_t>::max();
  if (seconds < 0 ||
      static_cast<std::uint64_t>(seconds) > (kLimit - fraction) / kNsPerSecond)
    return false;
  output = static_cast<std::uint64_t>(seconds) * kNsPerSecond + fraction;
  return true;
}

}  // namespace

base::Status LoadDolphinDbCsv(const ReplayConfig& config, ReplayData& output,
                              std::span<char> error_detail) noexcept {
  if (!error_detail.empty()) error_detail.front() = '\0';
  if (config.path[0] == '\0' ||
      std::memchr(config.path, '\0', sizeof(config.path)) == nullptr ||
      config.source_id.value == 0 || config.price_decimals > 6 ||
      config.max_records == 0 || config.max_records > kMaxReplayRecords)
    return Failure(error_detail, 0, "invalid replay configuration",
                   base::ErrorCode::kInvalidArgument);
  ReplayData parsed;
  auto status = parsed.Reserve(config.max_records);
  if (!status.ok())
    return Failure(error_detail, 0, "cannot allocate replay storage",
                   status.code);
  // Both supported venues have exactly one million six-digit symbol encodings;
  // a direct counter table bounds memory and avoids per-record allocations.
  std::unique_ptr<std::uint32_t[]> sequences(
      new (std::nothrow) std::uint32_t[2 * kSymbolsPerVenue]());
  if (!sequences)
    return Failure(error_detail, 0, "cannot allocate sequence counters",
                   base::ErrorCode::kResourceExhausted);
  // Nonblocking open permits rejecting a FIFO without waiting for a writer.
  // The flag does not change regular-file read semantics.
  const int fd = ::open(config.path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0)
    return Failure(error_detail, 0, "cannot open input file",
                   base::ErrorCode::kIoError, errno);
  struct stat metadata{};
  if (::fstat(fd, &metadata) != 0) {
    const int error = errno;
    (void)::close(fd);
    return Failure(error_detail, 0, "cannot stat input file",
                   base::ErrorCode::kIoError, error);
  }
  if (!S_ISREG(metadata.st_mode)) {
    (void)::close(fd);
    return Failure(error_detail, 0, "input must be a regular file",
                   base::ErrorCode::kInvalidArgument);
  }
  std::FILE* file = ::fdopen(fd, "rb");
  if (file == nullptr) {
    const int error = errno;
    (void)::close(fd);
    return Failure(error_detail, 0, "cannot create input stream",
                   base::ErrorCode::kIoError, error);
  }
  // The shared cleanup path closes the file on every parse failure. Only a
  // complete successful read and close can replace the caller's output.
  const auto load = [&]() noexcept -> base::Status {
    CsvReader reader(file);
    std::array<char, kMaxCsvRecordBytes> record;
    std::array<std::string_view, kMaxCsvColumns> fields;
    std::array<std::size_t, kRequiredColumns.size()> columns;
    bool have_header = false;
    std::size_t header_count = 0;
    std::size_t receive_column = kMaxCsvColumns;
    std::uint64_t first_time = 0;
    std::uint64_t last_offset = 0;
    for (;;) {
      std::size_t length, line;
      const auto read_result = reader.Next(record, length, line);
      if (read_result == CsvReader::Result::kEnd) break;
      if (read_result == CsvReader::Result::kIoError)
        return Failure(error_detail, line, "cannot read input file",
                       base::ErrorCode::kIoError, errno == 0 ? EIO : errno);
      if (read_result == CsvReader::Result::kTooLong)
        return Failure(error_detail, line, "record exceeds 64 KiB limit");
      if (read_result == CsvReader::Result::kInvalid)
        return Failure(error_detail, line, "record contains a NUL byte");
      std::span<char> encoded_record(record.data(), length);
      if (!have_header && encoded_record.size() >= 3 &&
          std::memcmp(encoded_record.data(), "\xef\xbb\xbf", 3) == 0)
        encoded_record = encoded_record.subspan(3);
      if (Trim({encoded_record.data(), encoded_record.size()}).empty())
        continue;
      std::size_t count;
      if (!ParseRecord(encoded_record, fields, count))
        return Failure(error_detail, line,
                       "invalid quoting or too many columns");
      if (!have_header) {
        columns.fill(kMaxCsvColumns);
        for (std::size_t i = 0; i < count; ++i) {
          const auto name = Trim(fields[i]);
          if (name.empty())
            return Failure(error_detail, line, "empty column name");
          for (std::size_t j = 0; j < i; ++j) {
            if (name == Trim(fields[j]))
              return Failure(error_detail, line, "duplicate column name");
          }
          for (std::size_t j = 0; j < kRequiredColumns.size(); ++j) {
            if (name == kRequiredColumns[j]) columns[j] = i;
          }
          if (name == "RecvTime") receive_column = i;
        }
        for (const auto column : columns) {
          if (column == kMaxCsvColumns)
            return Failure(error_detail, line, "missing required column");
        }
        header_count = count;
        have_header = true;
        continue;
      }
      if (count != header_count)
        return Failure(error_detail, line, "column count differs from header");
      if (parsed.size() == config.max_records)
        return Failure(error_detail, line, "max_records exceeded",
                       base::ErrorCode::kResourceExhausted);
      ReplayRecord item;
      auto& quote = item.quote;
      quote.source_id = config.source_id;
      const auto venue = Trim(fields[columns[3]]);
      const auto symbol = Trim(fields[columns[2]]);
      if ((venue != "101" && venue != "102") ||
          model::MakeInstrumentId(venue == "101" ? "SSE" : "SZSE", symbol,
                                  quote.instrument_id) != base::ErrorCode::kOk)
        return Failure(error_detail, line, "invalid security ID or source");
      if (!ParseTimestamp(fields[columns[0]], fields[columns[1]],
                          quote.event_time_ns))
        return Failure(error_detail, line, "invalid Date or Time");
      std::uint64_t arrival_time = quote.event_time_ns;
      if (receive_column != kMaxCsvColumns) {
        const auto receive = Trim(fields[receive_column]);
        if (receive.size() < 19 || receive[10] != 'T' ||
            !ParseTimestamp(receive.substr(0, 10), receive.substr(11),
                            arrival_time))
          return Failure(error_detail, line, "invalid RecvTime");
      }
      if (!ParsePrice(fields[columns[4]], config.price_decimals,
                      quote.ask_price) ||
          !ParsePrice(fields[columns[5]], config.price_decimals,
                      quote.bid_price))
        return Failure(error_detail, line, "invalid or overflowing price");
      std::uint64_t ask_qty, bid_qty;
      constexpr auto kQtyLimit =
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
      if (!ParseUnsigned(Trim(fields[columns[6]]), kQtyLimit, ask_qty) ||
          !ParseUnsigned(Trim(fields[columns[7]]), kQtyLimit, bid_qty))
        return Failure(error_detail, line, "invalid or overflowing quantity");
      quote.ask_qty = static_cast<std::int64_t>(ask_qty);
      quote.bid_qty = static_cast<std::int64_t>(bid_qty);
      quote.flags =
          static_cast<std::uint8_t>(model::QuoteFlag::kEventTimeValid);
      if (ask_qty != 0)
        quote.flags |= static_cast<std::uint8_t>(model::QuoteFlag::kAskValid);
      else
        quote.ask_price = 0;
      if (bid_qty != 0)
        quote.flags |= static_cast<std::uint8_t>(model::QuoteFlag::kBidValid);
      else
        quote.bid_price = 0;
      const auto symbol_value =
          static_cast<std::uint32_t>(quote.instrument_id.value);
      const auto index = symbol_value + (venue == "102" ? kSymbolsPerVenue : 0);
      quote.sequence = ++sequences[index];
      if (parsed.size() == 0) first_time = arrival_time;
      const auto offset =
          arrival_time >= first_time ? arrival_time - first_time : 0;
      item.offset_ns = std::max(last_offset, offset);
      last_offset = item.offset_ns;
      const auto append_status = parsed.Append(item);
      if (!append_status.ok())
        return Failure(error_detail, line, "cannot append replay record",
                       append_status.code);
    }
    if (!have_header) return Failure(error_detail, 0, "missing CSV header");
    return {};
  };
  status = load();
  const int close_result = std::fclose(file);
  if (!status.ok()) return status;
  if (close_result != 0)
    return Failure(error_detail, 0, "cannot close input file",
                   base::ErrorCode::kIoError, errno);
  output = std::move(parsed);
  return {};
}

}  // namespace yquant::marketd
