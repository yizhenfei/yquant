#include "yquant/base/spsc_queue.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

#include "base/internal/spsc_queue_layout.h"

namespace yquant::base {
namespace {

using internal::kSpscDataOffset;
using internal::kSpscFrameHeaderBytes;
using internal::kSpscWrapMarker;
using internal::SpscControlBlock;
using internal::SpscMetadata;

static_assert(SpscQueue::required_alignment() == internal::kSpscAlignment);

constexpr bool PlatformSupported() noexcept {
  return std::atomic<std::uint64_t>::is_always_lock_free;
}

SpscStatus CheckStorage(std::span<std::byte> memory) noexcept {
  if (memory.data() == nullptr) return SpscStatus::kInvalidArgument;
  if (reinterpret_cast<std::uintptr_t>(memory.data()) %
          internal::kSpscAlignment !=
      0) {
    return SpscStatus::kMisalignedStorage;
  }
  if (memory.size() < kSpscDataOffset) {
    return SpscStatus::kInsufficientStorage;
  }
  return SpscStatus::kSuccess;
}

void EncodeLength(std::byte* output, std::uint32_t length) noexcept {
  for (unsigned int i = 0; i < kSpscFrameHeaderBytes; ++i) {
    output[i] = static_cast<std::byte>(length & 0xff);
    length >>= 8;
  }
}

std::uint32_t DecodeLength(const std::byte* input) noexcept {
  std::uint32_t length = 0;
  for (unsigned int i = 0; i < kSpscFrameHeaderBytes; ++i) {
    length |= std::to_integer<std::uint32_t>(input[i]) << (8 * i);
  }
  return length;
}

}  // namespace

SpscQueue::~SpscQueue() noexcept {
  if (borrow_active_) {
    assert(!borrow_active_ && "SpscQueue destroyed with an active borrow");
    std::abort();
  }
}

SpscQueue::SizeResult SpscQueue::RequiredBytes(
    std::uint64_t capacity) noexcept {
  if (capacity < kSpscFrameHeaderBytes || !std::has_single_bit(capacity) ||
      capacity >= (std::uint64_t{1} << 63) ||
      capacity > std::numeric_limits<std::size_t>::max() - kSpscDataOffset) {
    return {SpscStatus::kInvalidArgument, 0};
  }
  return {SpscStatus::kSuccess,
          kSpscDataOffset + static_cast<std::size_t>(capacity)};
}

SpscStatus SpscQueue::Initialize(std::span<std::byte> memory,
                                 std::uint64_t capacity) noexcept {
  auto required = RequiredBytes(capacity);
  if (required.status != SpscStatus::kSuccess) return required.status;
  auto storage_status = CheckStorage(memory);
  if (storage_status != SpscStatus::kSuccess) return storage_status;
  if (memory.size() < required.bytes) {
    return SpscStatus::kInsufficientStorage;
  }
  if (!PlatformSupported()) return SpscStatus::kUnsupportedPlatform;

  auto* control = ::new (static_cast<void*>(memory.data())) SpscControlBlock{};
  control->metadata.magic = internal::kSpscMagic;
  control->metadata.version = internal::kSpscLayoutVersion;
  control->metadata.data_offset = kSpscDataOffset;
  control->metadata.capacity = capacity;
  return SpscStatus::kSuccess;
}

SpscStatus SpscQueue::Attach(std::span<std::byte> memory, Role role) noexcept {
  if (attached()) return SpscStatus::kInvalidState;
  if (role != Role::kProducer && role != Role::kConsumer) {
    return SpscStatus::kInvalidArgument;
  }
  auto storage_status = CheckStorage(memory);
  if (storage_status != SpscStatus::kSuccess) return storage_status;
  if (!PlatformSupported()) return SpscStatus::kUnsupportedPlatform;

  // Read only immutable metadata until the layout has been validated. In
  // particular, independent cursor reads would not form a coherent snapshot.
  SpscMetadata metadata;
  std::memcpy(&metadata, memory.data(), sizeof(metadata));
  if (metadata.magic != internal::kSpscMagic ||
      metadata.version != internal::kSpscLayoutVersion ||
      metadata.data_offset != kSpscDataOffset) {
    return SpscStatus::kIncompatibleLayout;
  }
  auto required = RequiredBytes(metadata.capacity);
  if (required.status != SpscStatus::kSuccess) {
    return SpscStatus::kIncompatibleLayout;
  }
  if (memory.size() < required.bytes) {
    return SpscStatus::kInsufficientStorage;
  }

  control_ = std::launder(reinterpret_cast<SpscControlBlock*>(memory.data()));
  data_ = memory.data() + kSpscDataOffset;
  capacity_ = metadata.capacity;
  role_ = role;
  return SpscStatus::kSuccess;
}

SpscStatus SpscQueue::Detach() noexcept {
  if (borrow_active_) return SpscStatus::kBusy;
  control_ = nullptr;
  data_ = nullptr;
  capacity_ = 0;
  role_ = Role::kUnbound;
  return SpscStatus::kSuccess;
}

std::size_t SpscQueue::max_payload_size() const noexcept {
  if (!attached()) return 0;
  return static_cast<std::size_t>(std::min<std::uint64_t>(
      capacity_ - kSpscFrameHeaderBytes, kSpscWrapMarker - 1));
}

SpscStatus SpscQueue::CheckWrite(std::size_t payload_size) const noexcept {
  if (role_ != Role::kProducer) return SpscStatus::kInvalidState;
  if (borrow_active_) return SpscStatus::kBusy;
  if (payload_size > max_payload_size()) return SpscStatus::kTooLarge;
  auto w = control_->write.position.load(std::memory_order_relaxed);
  auto r = control_->read.position.load(std::memory_order_acquire);
  auto used = w - r;
  if (used > capacity_) return SpscStatus::kCorrupt;
  return kSpscFrameHeaderBytes + static_cast<std::uint64_t>(payload_size) <=
                 capacity_ - used
             ? SpscStatus::kSuccess
             : SpscStatus::kFull;
}

SpscQueue::ReserveResult SpscQueue::TryReserve(
    std::size_t max_payload_size) noexcept {
  if (role_ != Role::kProducer) return {SpscStatus::kInvalidState, {}};
  if (borrow_active_) return {SpscStatus::kBusy, {}};
  if (max_payload_size > this->max_payload_size()) {
    return {SpscStatus::kTooLarge, {}};
  }
  auto w = control_->write.position.load(std::memory_order_relaxed);
  auto r = control_->read.position.load(std::memory_order_acquire);
  auto used = w - r;
  if (used > capacity_) return {SpscStatus::kCorrupt, {}};
  auto free = capacity_ - used;
  auto frame_bytes =
      kSpscFrameHeaderBytes + static_cast<std::uint64_t>(max_payload_size);
  auto offset = w & (capacity_ - 1);
  auto tail = capacity_ - offset;
  if (frame_bytes > tail) {
    if (tail > free) return {SpscStatus::kFull, {}};
    if (tail >= kSpscFrameHeaderBytes) {
      EncodeLength(data_ + static_cast<std::size_t>(offset), kSpscWrapMarker);
    }
    // Publish padding independently: an empty queue at a nonzero offset must
    // let the consumer return the tail before a maximum-size frame can fit.
    control_->write.position.store(w + tail, std::memory_order_release);
    w += tail;
    free -= tail;
  }
  if (frame_bytes > free) return {SpscStatus::kFull, {}};
  borrow_active_ = true;
  return {SpscStatus::kSuccess, WriteReservation(this, w, max_payload_size)};
}

SpscQueue::AcquireResult SpscQueue::TryAcquire() noexcept {
  if (role_ != Role::kConsumer) return {SpscStatus::kInvalidState, {}};
  if (borrow_active_) return {SpscStatus::kBusy, {}};
  auto r = control_->read.position.load(std::memory_order_relaxed);
  auto w = control_->write.position.load(std::memory_order_acquire);
  auto used = w - r;
  if (used > capacity_) return {SpscStatus::kCorrupt, {}};
  if (used == 0) return {SpscStatus::kEmpty, {}};

  auto offset = r & (capacity_ - 1);
  auto tail = capacity_ - offset;
  bool padding = tail < kSpscFrameHeaderBytes;
  if (padding) {
    if (used < tail) return {SpscStatus::kCorrupt, {}};
  } else {
    if (used < kSpscFrameHeaderBytes) return {SpscStatus::kCorrupt, {}};
    padding = DecodeLength(data_ + static_cast<std::size_t>(offset)) ==
              kSpscWrapMarker;
    if (padding && (offset == 0 || used < tail)) {
      return {SpscStatus::kCorrupt, {}};
    }
  }
  if (padding) {
    control_->read.position.store(r + tail, std::memory_order_release);
    r += tail;
    offset = 0;
    tail = capacity_;
    w = control_->write.position.load(std::memory_order_acquire);
    used = w - r;
    if (used > capacity_) return {SpscStatus::kCorrupt, {}};
    if (used == 0) return {SpscStatus::kEmpty, {}};
  }
  if (used < kSpscFrameHeaderBytes) return {SpscStatus::kCorrupt, {}};
  auto payload_size = DecodeLength(data_ + static_cast<std::size_t>(offset));
  auto frame_bytes =
      kSpscFrameHeaderBytes + static_cast<std::uint64_t>(payload_size);
  if (payload_size == kSpscWrapMarker || payload_size > max_payload_size() ||
      frame_bytes > tail || frame_bytes > used) {
    return {SpscStatus::kCorrupt, {}};
  }
  borrow_active_ = true;
  return {SpscStatus::kSuccess, ReadLease(this, r, payload_size)};
}

SpscStatus SpscQueue::TryWrite(std::span<const std::byte> payload) noexcept {
  auto result = TryReserve(payload.size());
  if (result.status != SpscStatus::kSuccess) return result.status;
  if (!payload.empty()) {
    std::memcpy(result.reservation.payload().data(), payload.data(),
                payload.size());
  }
  return result.reservation.Commit(payload.size());
}

SpscQueue::ReadResult SpscQueue::TryRead(std::span<std::byte> output) noexcept {
  auto result = TryAcquire();
  if (result.status != SpscStatus::kSuccess) return {result.status, 0};
  auto size = result.lease.size();
  if (output.size() < size) {
    return {SpscStatus::kBufferTooSmall, size};
  }
  if (size != 0) {
    std::memcpy(output.data(), result.lease.payload().data(), size);
  }
  auto status = result.lease.Release();
  return {status, size};
}

SpscQueue::WriteReservation::WriteReservation(
    SpscQueue* owner, std::uint64_t frame_pos,
    std::size_t reserved_size) noexcept
    : owner_(owner), frame_pos_(frame_pos), reserved_size_(reserved_size) {}

SpscQueue::WriteReservation::~WriteReservation() noexcept { Cancel(); }

SpscQueue::WriteReservation::WriteReservation(WriteReservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      frame_pos_(std::exchange(other.frame_pos_, 0)),
      reserved_size_(std::exchange(other.reserved_size_, 0)) {}

SpscQueue::WriteReservation& SpscQueue::WriteReservation::operator=(
    WriteReservation&& other) noexcept {
  if (this != &other) {
    Cancel();
    owner_ = std::exchange(other.owner_, nullptr);
    frame_pos_ = std::exchange(other.frame_pos_, 0);
    reserved_size_ = std::exchange(other.reserved_size_, 0);
  }
  return *this;
}

std::span<std::byte> SpscQueue::WriteReservation::payload() const noexcept {
  if (!active()) return {};
  auto offset = static_cast<std::size_t>(frame_pos_ & (owner_->capacity_ - 1));
  return {owner_->data_ + offset + kSpscFrameHeaderBytes, reserved_size_};
}

SpscStatus SpscQueue::WriteReservation::Commit(
    std::size_t actual_size) noexcept {
  if (!active()) return SpscStatus::kInvalidState;
  if (actual_size > reserved_size_) return SpscStatus::kInvalidSize;
  auto offset = static_cast<std::size_t>(frame_pos_ & (owner_->capacity_ - 1));
  EncodeLength(owner_->data_ + offset, static_cast<std::uint32_t>(actual_size));
  owner_->control_->write.position.store(
      frame_pos_ + kSpscFrameHeaderBytes + actual_size,
      std::memory_order_release);
  Cancel();
  return SpscStatus::kSuccess;
}

void SpscQueue::WriteReservation::Cancel() noexcept {
  if (owner_ != nullptr) owner_->borrow_active_ = false;
  owner_ = nullptr;
  frame_pos_ = 0;
  reserved_size_ = 0;
}

SpscQueue::ReadLease::ReadLease(SpscQueue* owner, std::uint64_t frame_pos,
                                std::size_t payload_size) noexcept
    : owner_(owner), frame_pos_(frame_pos), payload_size_(payload_size) {}

SpscQueue::ReadLease::~ReadLease() noexcept { Cancel(); }

SpscQueue::ReadLease::ReadLease(ReadLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      frame_pos_(std::exchange(other.frame_pos_, 0)),
      payload_size_(std::exchange(other.payload_size_, 0)) {}

SpscQueue::ReadLease& SpscQueue::ReadLease::operator=(
    ReadLease&& other) noexcept {
  if (this != &other) {
    Cancel();
    owner_ = std::exchange(other.owner_, nullptr);
    frame_pos_ = std::exchange(other.frame_pos_, 0);
    payload_size_ = std::exchange(other.payload_size_, 0);
  }
  return *this;
}

std::span<const std::byte> SpscQueue::ReadLease::payload() const noexcept {
  if (!active()) return {};
  auto offset = static_cast<std::size_t>(frame_pos_ & (owner_->capacity_ - 1));
  return {owner_->data_ + offset + kSpscFrameHeaderBytes, payload_size_};
}

std::size_t SpscQueue::ReadLease::size() const noexcept {
  return active() ? payload_size_ : 0;
}

SpscStatus SpscQueue::ReadLease::Release() noexcept {
  if (!active()) return SpscStatus::kInvalidState;
  owner_->control_->read.position.store(
      frame_pos_ + kSpscFrameHeaderBytes + payload_size_,
      std::memory_order_release);
  Cancel();
  return SpscStatus::kSuccess;
}

void SpscQueue::ReadLease::Cancel() noexcept {
  if (owner_ != nullptr) owner_->borrow_active_ = false;
  owner_ = nullptr;
  frame_pos_ = 0;
  payload_size_ = 0;
}

}  // namespace yquant::base
