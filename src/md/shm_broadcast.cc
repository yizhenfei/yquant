#include "yquant/md/shm_broadcast.h"

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <chrono>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <utility>

#include "md/internal/event_codec.h"

namespace yquant::md {
namespace {
constexpr size_t kWordBytes = sizeof(uint64_t);
constexpr size_t kMaxWordCount =
    (internal::kWireHeaderSize + kMaxEventPayloadSize + kWordBytes - 1) /
    kWordBytes;
base::Status Error(base::ErrorCode code) { return {code, {}}; }

// Reserve room for the event envelope in each byte record.
base::ShmBroadcastConfig ByteConfig(const PublishConfig& config) {
  return {
      config.endpoint,
      {config.capacity, internal::kWireHeaderSize + config.max_payload_size}};
}

// Queue status belongs to the transport; only this adapter calls records
// events.
ReadResult EventResult(const base::ShmBroadcastReadResult& result) {
  const auto& read = result.read;
  ReadResult out;
  out.generation = read.generation;
  out.sequence = read.sequence;
  out.attempts = read.attempts;
  out.gap = read.gap;
  out.error = read.error;
  if (result.stream_changed) {
    out.status = ReadStatus::kStreamChanged;
    return out;
  }
  switch (read.status) {
    case base::BroadcastReadStatus::kRecord:
      out.status = ReadStatus::kEvent;
      break;
    case base::BroadcastReadStatus::kEmpty:
      out.status = ReadStatus::kEmpty;
      break;
    case base::BroadcastReadStatus::kRetry:
      out.status = ReadStatus::kRetry;
      break;
    case base::BroadcastReadStatus::kGap:
      out.status = ReadStatus::kGap;
      break;
    case base::BroadcastReadStatus::kEnd:
      out.status = ReadStatus::kEndOfGeneration;
      break;
    case base::BroadcastReadStatus::kDetached:
      out.status = ReadStatus::kClosed;
      break;
    case base::BroadcastReadStatus::kError:
      out.status = ReadStatus::kError;
      break;
  }
  return out;
}
}  // namespace
base::Status ValidatePublishConfig(const PublishConfig& config) noexcept {
  if (config.max_payload_size > kMaxEventPayloadSize) {
    return Error(base::ErrorCode::kInvalidArgument);
  }
  return base::ValidateShmBroadcastConfig(ByteConfig(config));
}
struct ShmPublisher::Impl {
  base::ShmBroadcastWriter writer;
  std::array<uint64_t, kMaxWordCount> encoded{};
};
ShmPublisher::ShmPublisher() noexcept = default;
ShmPublisher::ShmPublisher(ShmPublisher&&) noexcept = default;
ShmPublisher& ShmPublisher::operator=(ShmPublisher&&) noexcept = default;
ShmPublisher::~ShmPublisher() = default;
base::Status ShmPublisher::Create(const PublishConfig& config) {
  if (impl_) {
    return Error(base::ErrorCode::kInvalidState);
  }
  auto status = ValidatePublishConfig(config);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) {
    return Error(base::ErrorCode::kResourceExhausted);
  }
  status = impl->writer.Create(ByteConfig(config));
  if (!status.ok()) {
    return status;
  }
  impl_ = std::move(impl);
  return {};
}
base::Status ShmPublisher::Publish(EventView event, uint64_t* sequence) {
  if (!impl_) {
    return Error(base::ErrorCode::kInvalidState);
  }
  const auto bytes = impl_->writer.max_record_size();
  auto words =
      std::span(impl_->encoded).first((bytes + kWordBytes - 1) / kWordBytes);
  auto encoded = internal::EncodeEvent(event, words, max_payload_size());
  if (!encoded) {
    return encoded.error();
  }
  return impl_->writer.TryWrite(std::as_bytes(words).first(bytes), sequence);
}
base::Status ShmPublisher::Tick() {
  return impl_ ? impl_->writer.Tick() : Error(base::ErrorCode::kInvalidState);
}
Generation ShmPublisher::generation() const noexcept {
  return impl_ ? impl_->writer.generation() : Generation{};
}
size_t ShmPublisher::capacity() const noexcept {
  return impl_ ? impl_->writer.capacity() : 0;
}
size_t ShmPublisher::max_payload_size() const noexcept {
  return impl_ ? impl_->writer.max_record_size() - internal::kWireHeaderSize
               : 0;
}
void ShmPublisher::Close() noexcept { impl_.reset(); }

struct ShmReader::Impl {
  // Storage precedes the transport so transport detaches before it is freed.
  std::array<uint64_t, kMaxWordCount> words{};
  base::ShmBroadcastReader reader;
  std::optional<ReadResult> invalid_event;
  bool stream_changed = false;
};
ShmReader::ShmReader() noexcept = default;
ShmReader::ShmReader(ShmReader&&) noexcept = default;
ShmReader& ShmReader::operator=(ShmReader&&) noexcept = default;
ShmReader::~ShmReader() = default;
base::Status ShmReader::Open(const Endpoint& endpoint, StartPosition start) {
  if (impl_) {
    return Error(base::ErrorCode::kInvalidState);
  }
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) {
    return Error(base::ErrorCode::kResourceExhausted);
  }
  auto status = impl->reader.Open(
      endpoint, std::as_writable_bytes(std::span(impl->words)), start);
  if (!status.ok()) {
    return status;
  }
  const auto bytes = impl->reader.max_record_size();
  if (bytes < internal::kWireHeaderSize ||
      bytes > internal::kWireHeaderSize + kMaxEventPayloadSize) {
    return Error(base::ErrorCode::kCorrupt);
  }
  impl_ = std::move(impl);
  return {};
}
ReadResult ShmReader::TryNext(EventBuffer& buffer, ReadBudget budget) {
  if (!impl_) {
    ReadResult result;
    result.status = ReadStatus::kClosed;
    return result;
  }
  if (!impl_->stream_changed && impl_->invalid_event) {
    return *impl_->invalid_event;
  }
  const auto raw = impl_->reader.TryRead(budget);
  auto result = EventResult(raw);
  if (result.status != ReadStatus::kEvent) {
    return result;
  }
  const auto bytes = impl_->reader.max_record_size();
  base::Status status;
  if (raw.read.record.size() != bytes) {
    status = Error(base::ErrorCode::kCorrupt);
  } else {
    const auto words =
        std::span(impl_->words).first((bytes + kWordBytes - 1) / kWordBytes);
    auto decoded = internal::DecodeEvent(words, max_payload_size(), buffer);
    if (!decoded) {
      status = decoded.error();
    }
  }
  if (!status.ok()) {
    result.status = ReadStatus::kError;
    result.error = status;
    impl_->invalid_event = result;
  }
  return result;
}
base::Status ShmReader::Resume(const GapToken& token, uint64_t& sequence,
                               ResumePosition position) {
  return impl_ ? impl_->reader.Resume(token, sequence, position)
               : Error(base::ErrorCode::kInvalidState);
}
base::Status ShmReader::Refresh(RefreshResult& result,
                                std::chrono::nanoseconds stale_after) {
  if (!impl_) {
    return Error(base::ErrorCode::kInvalidState);
  }
  auto status = impl_->reader.Refresh(result, stale_after);
  if (status.ok() && result.status == RefreshStatus::kStreamChanged) {
    impl_->stream_changed = true;
  }
  return status;
}
Generation ShmReader::generation() const noexcept {
  return impl_ ? impl_->reader.generation() : Generation{};
}
uint64_t ShmReader::next_sequence() const noexcept {
  if (!impl_) {
    return 0;
  }
  return impl_->invalid_event ? impl_->invalid_event->sequence
                              : impl_->reader.next_sequence();
}
size_t ShmReader::max_payload_size() const noexcept {
  return impl_ ? impl_->reader.max_record_size() - internal::kWireHeaderSize
               : 0;
}
void ShmReader::Close() noexcept { impl_.reset(); }
}  // namespace yquant::md
