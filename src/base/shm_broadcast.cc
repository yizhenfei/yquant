#include "yquant/base/shm_broadcast.h"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "base/internal/shared_memory_mapping.h"
#include "base/internal/shm_broadcast_layout.h"
#include "base/internal/shm_stream_registry.h"

namespace yquant::base {
namespace {
using internal::SharedMemoryMapping;
using internal::ShmBroadcastControl;
using internal::ShmBroadcastLayout;
using internal::ShmStreamRegistry;
static_assert(std::is_standard_layout_v<ShmBroadcastControl>);
static_assert(offsetof(ShmBroadcastControl, layout) == 0);
static_assert(sizeof(ShmBroadcastControl) %
                  BroadcastQueue::required_alignment() ==
              0);
constexpr uint64_t kMagic = 0x595153484d425243ULL;
constexpr uint64_t kVersion = 2;
constexpr unsigned kDiscoveryAttempts = 4;
constexpr unsigned kNameAttempts = 16;

Status Error(ErrorCode code) { return {code, {}}; }

// Envelope dimensions are independent of the queue's slot layout. Page
// rounding and the SHM allocation budget apply only to the outer mapping.
Status MakeLayout(size_t queue_bytes, BroadcastId id,
                  ShmBroadcastLayout& layout) noexcept {
  if (queue_bytes >
      std::numeric_limits<size_t>::max() - sizeof(ShmBroadcastControl)) {
    return Error(ErrorCode::kInvalidArgument);
  }
  size_t bytes;
  auto status = internal::SharedMemoryBytes(
      sizeof(ShmBroadcastControl) + queue_bytes, bytes);
  if (!status.ok()) {
    return status;
  }
  layout = {kMagic,
            kVersion,
            bytes,
            sizeof(ShmBroadcastControl),
            queue_bytes,
            sizeof(std::atomic<uint64_t>),
            alignof(std::atomic<uint64_t>),
            offsetof(ShmBroadcastControl, heartbeat_ns),
            id};
  return {};
}

// Only inspect plain immutable bytes until layout and heartbeat offsets have
// been checked. Queue Attach separately checks its own atomic ABI/layout.
Status ValidateEnvelope(std::span<std::byte> memory, BroadcastId id,
                        ShmBroadcastLayout& layout) {
  if (memory.size() < sizeof(ShmBroadcastControl) ||
      reinterpret_cast<uintptr_t>(memory.data()) %
          alignof(ShmBroadcastControl)) {
    return Error(ErrorCode::kCorrupt);
  }
  std::memcpy(&layout, memory.data(), sizeof(layout));
  if (layout.queue_bytes > std::numeric_limits<size_t>::max()) {
    return Error(ErrorCode::kCorrupt);
  }
  ShmBroadcastLayout expected;
  if (!MakeLayout(layout.queue_bytes, id, expected).ok() ||
      layout != expected || layout.bytes != memory.size()) {
    return Error(ErrorCode::kCorrupt);
  }
  return {};
}
}  // namespace

Status ValidateShmBroadcastConfig(const ShmBroadcastConfig& config) noexcept {
  auto endpoint = internal::ValidateShmEndpoint(config.endpoint);
  if (!endpoint) {
    return endpoint.error();
  }
  const auto size = BroadcastQueue::RequiredBytes(config.queue);
  if (!size.status.ok()) {
    return size.status;
  }
  ShmBroadcastLayout layout;
  return MakeLayout(size.bytes, {}, layout);
}

struct ShmBroadcastWriter::Impl {
  // Registry is destroyed last so the stable lock survives all other cleanup.
  ShmStreamRegistry registry;
  SharedMemoryMapping mapping;
  BroadcastWriter writer;
  ShmBroadcastControl* control = nullptr;
  ~Impl() {
    if (registry.owned_by_current_process() && writer.queue().attached()) {
      (void)writer.Finish();
    }
    writer.Detach();
    registry.Retire();
    mapping.Close();
  }
};
ShmBroadcastWriter::ShmBroadcastWriter() noexcept = default;
ShmBroadcastWriter::ShmBroadcastWriter(ShmBroadcastWriter&&) noexcept = default;
ShmBroadcastWriter& ShmBroadcastWriter::operator=(
    ShmBroadcastWriter&&) noexcept = default;
ShmBroadcastWriter::~ShmBroadcastWriter() = default;
Status ShmBroadcastWriter::Create(const ShmBroadcastConfig& config) {
  if (impl_) {
    return Error(ErrorCode::kInvalidState);
  }
  auto status = ValidateShmBroadcastConfig(config);
  if (!status.ok()) {
    return status;
  }
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) {
    return Error(ErrorCode::kResourceExhausted);
  }
  status = impl->registry.Acquire(config.endpoint);
  if (!status.ok()) {
    return status;
  }
  const auto required = BroadcastQueue::RequiredBytes(config.queue);
  ShmBroadcastLayout layout;
  for (unsigned attempt = 0; attempt < kNameAttempts; ++attempt) {
    status = impl->registry.Prepare();
    if (!status.ok()) {
      return status;
    }
    status = MakeLayout(required.bytes, impl->registry.manifest().generation,
                        layout);
    if (!status.ok()) {
      return status;
    }
    status =
        impl->mapping.Create(impl->registry.manifest().shm_name, layout.bytes);
    if (status.ok()) {
      break;
    }
    if (status.system_error.value() != EEXIST) {
      return status;
    }
  }
  if (!status.ok()) {
    return status;
  }
  auto memory = impl->mapping.memory();
  impl->control = ::new (memory.data()) ShmBroadcastControl{};
  impl->control->layout = layout;
  auto queue_memory = memory.subspan(layout.queue_offset, layout.queue_bytes);
  status =
      BroadcastQueue::Initialize(queue_memory, config.queue, layout.generation);
  if (!status.ok()) {
    return status;
  }
  status = impl->writer.Attach(queue_memory);
  if (!status.ok()) {
    return status;
  }
  impl->control->heartbeat_ns.store(internal::MonotonicNs(),
                                    std::memory_order_release);
  status = impl->registry.Publish();
  if (!status.ok()) {
    return status;
  }
  impl_ = std::move(impl);
  return {};
}
Status ShmBroadcastWriter::TryWrite(std::span<const std::byte> record,
                                    uint64_t* sequence) noexcept {
  if (!impl_ || !impl_->registry.owned_by_current_process()) {
    return Error(ErrorCode::kInvalidState);
  }
  return impl_->writer.TryWrite(record, sequence);
}
Status ShmBroadcastWriter::Tick() noexcept {
  if (!impl_ || !impl_->registry.owned_by_current_process()) {
    return Error(ErrorCode::kInvalidState);
  }
  impl_->control->heartbeat_ns.store(internal::MonotonicNs(),
                                     std::memory_order_release);
  return {};
}
BroadcastId ShmBroadcastWriter::generation() const noexcept {
  return impl_ ? impl_->writer.queue().generation() : BroadcastId{};
}
size_t ShmBroadcastWriter::capacity() const noexcept {
  return impl_ ? impl_->writer.queue().capacity() : 0;
}
size_t ShmBroadcastWriter::max_record_size() const noexcept {
  return impl_ ? impl_->writer.queue().max_record_size() : 0;
}
void ShmBroadcastWriter::Close() noexcept { impl_.reset(); }

struct ShmBroadcastReader::Impl {
  SharedMemoryMapping mapping;
  BroadcastReader reader;
  ShmEndpoint endpoint;
  ShmBroadcastControl* control = nullptr;
  std::optional<BroadcastId> changed_generation;
};
ShmBroadcastReader::ShmBroadcastReader() noexcept = default;
ShmBroadcastReader::ShmBroadcastReader(ShmBroadcastReader&&) noexcept = default;
ShmBroadcastReader& ShmBroadcastReader::operator=(
    ShmBroadcastReader&&) noexcept = default;
ShmBroadcastReader::~ShmBroadcastReader() = default;
Status ShmBroadcastReader::Open(const ShmEndpoint& endpoint,
                                std::span<std::byte> scratch,
                                BroadcastStartPosition start) {
  if (impl_) {
    return Error(ErrorCode::kInvalidState);
  }
  auto checked = internal::ValidateShmEndpoint(endpoint);
  if (!checked) {
    return checked.error();
  }
  Status last = Error(ErrorCode::kInvalidState);
  for (unsigned attempt = 0; attempt < kDiscoveryAttempts; ++attempt) {
    auto manifest = internal::Discover(*checked);
    if (!manifest) {
      last = manifest.error();
      if (last.system_error.value() != ENOENT) {
        return last;
      }
      continue;
    }
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
    if (!impl) {
      return Error(ErrorCode::kResourceExhausted);
    }
    auto status = impl->mapping.Open(manifest->shm_name);
    if (!status.ok()) {
      last = status;
      if (last.system_error.value() != ENOENT) {
        return last;
      }
      continue;
    }
    auto memory = impl->mapping.memory();
    ShmBroadcastLayout layout;
    status = ValidateEnvelope(memory, manifest->generation, layout);
    if (!status.ok()) {
      return status;
    }
    auto queue_memory = memory.subspan(layout.queue_offset, layout.queue_bytes);
    BroadcastQueue view;
    status = view.Attach(queue_memory);
    if (!status.ok()) {
      return status;
    }
    const auto required = BroadcastQueue::RequiredBytes(
        {view.capacity(), view.max_record_size()});
    if (view.generation() != manifest->generation ||
        required.bytes != layout.queue_bytes) {
      return Error(ErrorCode::kCorrupt);
    }
    auto reader_id = internal::RandomBroadcastId();
    if (!reader_id) {
      return reader_id.error();
    }
    status = impl->reader.Attach(queue_memory, scratch, start, *reader_id);
    if (!status.ok()) {
      return status;
    }
    auto current = internal::Discover(*checked);
    if (!current || current->generation != manifest->generation ||
        current->shm_name != manifest->shm_name) {
      if (!current && current.error().system_error.value() != ENOENT) {
        return current.error();
      }
      continue;
    }
    status = impl->mapping.VerifyName();
    if (!status.ok()) {
      last = status;
      if (last.code == ErrorCode::kInvalidState ||
          last.system_error.value() == ENOENT) {
        continue;
      }
      return last;
    }
    impl->control = reinterpret_cast<ShmBroadcastControl*>(memory.data());
    impl->endpoint = std::move(*checked);
    impl_ = std::move(impl);
    return {};
  }
  return last;
}
ShmBroadcastReadResult ShmBroadcastReader::TryRead(
    BroadcastReadBudget budget) noexcept {
  if (!impl_) {
    BroadcastReadResult result;
    result.status = BroadcastReadStatus::kDetached;
    return {false, result};
  }
  if (impl_->changed_generation) {
    BroadcastReadResult result;
    result.generation = generation();
    result.sequence = next_sequence();
    return {true, result};
  }
  return {false, impl_->reader.TryRead(budget)};
}
Status ShmBroadcastReader::Resume(const BroadcastGapToken& token,
                                  uint64_t& sequence,
                                  BroadcastResumePosition position) noexcept {
  if (!impl_) {
    return Error(ErrorCode::kInvalidState);
  }
  if (impl_->changed_generation) {
    return Error(ErrorCode::kInvalidArgument);
  }
  return impl_->reader.Resume(token, sequence, position);
}
Status ShmBroadcastReader::Refresh(ShmRefreshResult& output,
                                   std::chrono::nanoseconds stale_after) {
  if (!impl_) {
    return Error(ErrorCode::kInvalidState);
  }
  if (stale_after < std::chrono::nanoseconds::zero()) {
    return Error(ErrorCode::kInvalidArgument);
  }
  ShmRefreshResult value;
  if (impl_->reader.queue().finished()) {
    value.health = ShmWriterHealth::kStopped;
  } else {
    const auto heartbeat =
        impl_->control->heartbeat_ns.load(std::memory_order_acquire);
    const auto now = internal::MonotonicNs();
    value.health =
        now >= heartbeat &&
                now - heartbeat > static_cast<uint64_t>(stale_after.count())
            ? ShmWriterHealth::kStale
            : ShmWriterHealth::kReady;
  }
  if (impl_->changed_generation) {
    value.status = ShmRefreshStatus::kStreamChanged;
    value.discovered_generation = impl_->changed_generation;
  } else {
    auto manifest = internal::Discover(impl_->endpoint);
    if (!manifest) {
      if (manifest.error().system_error.value() != ENOENT) {
        return manifest.error();
      }
      value.status = ShmRefreshStatus::kMissing;
    } else {
      value.discovered_generation = manifest->generation;
      if (manifest->generation != generation()) {
        impl_->changed_generation = manifest->generation;
        value.status = ShmRefreshStatus::kStreamChanged;
      }
    }
  }
  output = value;
  return {};
}
BroadcastId ShmBroadcastReader::generation() const noexcept {
  return impl_ ? impl_->reader.queue().generation() : BroadcastId{};
}
uint64_t ShmBroadcastReader::next_sequence() const noexcept {
  return impl_ ? impl_->reader.next_sequence() : 0;
}
size_t ShmBroadcastReader::max_record_size() const noexcept {
  return impl_ ? impl_->reader.queue().max_record_size() : 0;
}
void ShmBroadcastReader::Close() noexcept { impl_.reset(); }
}  // namespace yquant::base
