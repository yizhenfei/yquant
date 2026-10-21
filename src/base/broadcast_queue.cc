#include "yquant/base/broadcast_queue.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

#include "base/internal/broadcast_layout.h"

namespace yquant::base {
namespace {
using internal::AtomicWord;
using internal::BroadcastHeader;
using internal::BroadcastLayout;
using internal::BroadcastSlot;
using internal::BroadcastState;
using internal::kMaxBroadcastSequence;
constexpr auto kAlignment = BroadcastQueue::required_alignment();
static_assert(std::is_standard_layout_v<BroadcastLayout>);
static_assert(std::is_standard_layout_v<BroadcastHeader>);
static_assert(offsetof(BroadcastHeader, layout) == 0);
static_assert(alignof(AtomicWord) <= kAlignment);
constexpr std::size_t kSlotMetadataWords = 2;
constexpr std::uint64_t kMagic = 0x595142524f414443ULL;
constexpr std::uint64_t kVersion = 1;
constexpr auto kReady = static_cast<std::uint64_t>(BroadcastState::kReady);
constexpr auto kFinished =
    static_cast<std::uint64_t>(BroadcastState::kFinished);
std::atomic<std::uint64_t> next_session{1};

Status Error(ErrorCode code) noexcept { return {code, {}}; }

// Checked size calculations leave out unchanged on overflow.
bool Add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (b > std::numeric_limits<std::size_t>::max() - a) return false;
  out = a + b;
  return true;
}
bool Multiply(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (a && b > std::numeric_limits<std::size_t>::max() / a) return false;
  out = a * b;
  return true;
}
bool Align(std::size_t value, std::size_t& out) noexcept {
  std::size_t sum;
  if (!Add(value, kAlignment - 1, sum)) return false;
  out = sum & ~(kAlignment - 1);
  return true;
}

// Reject overlapping borrowed storage before any mutation.
bool Overlaps(std::span<const std::byte> a,
              std::span<const std::byte> b) noexcept {
  if (a.empty() || b.empty()) return false;
  const auto x = reinterpret_cast<std::uintptr_t>(a.data());
  const auto y = reinterpret_cast<std::uintptr_t>(b.data());
  return x <= y ? y - x < a.size() : x - y < b.size();
}

// ABI fingerprint includes the standard library and all atomic access offsets.
constexpr std::uint64_t AtomicAbi() noexcept {
  std::uint64_t value = sizeof(AtomicWord) | (alignof(AtomicWord) << 8);
  const auto mix = [&](std::uint64_t part) {
    value = (value ^ part) * 1099511628211ULL;
  };
#ifdef _LIBCPP_VERSION
  mix(1);
  mix(_LIBCPP_VERSION);
#ifdef _LIBCPP_ABI_VERSION
  mix(_LIBCPP_ABI_VERSION);
#endif
#elif defined(__GLIBCXX__)
  mix(2);
  mix(__GLIBCXX__);
#ifdef _GLIBCXX_USE_CXX11_ABI
  mix(_GLIBCXX_USE_CXX11_ABI);
#endif
#else
  mix(3);
#endif
  mix(sizeof(BroadcastLayout));
  mix(sizeof(BroadcastHeader));
  mix(alignof(BroadcastHeader));
  mix(offsetof(BroadcastHeader, state));
  mix(offsetof(BroadcastHeader, published_sequence));
  return value;
}

// Pure layout calculation: no OS page size, event schema or process identity.
Status MakeLayout(BroadcastConfig config, BroadcastId id,
                  BroadcastLayout& layout) noexcept {
  if (!config.capacity || !std::has_single_bit(config.capacity))
    return Error(ErrorCode::kInvalidArgument);
  std::size_t rounded, count, slot_bytes, stride, offset, slots, total;
  if (!Add(config.max_record_size, sizeof(std::uint64_t) - 1, rounded))
    return Error(ErrorCode::kInvalidArgument);
  const auto words = rounded / sizeof(std::uint64_t);
  if (!Add(words, kSlotMetadataWords, count) ||
      !Multiply(count, sizeof(AtomicWord), slot_bytes) ||
      !Align(slot_bytes, stride) || !Align(sizeof(BroadcastHeader), offset) ||
      !Multiply(config.capacity, stride, slots) || !Add(offset, slots, total))
    return Error(ErrorCode::kInvalidArgument);
  layout = {kMagic,
            kVersion,
            AtomicAbi(),
            std::endian::native == std::endian::little ? 1ULL : 2ULL,
            total,
            config.capacity,
            config.max_record_size,
            words,
            stride,
            offset,
            id};
  return {};
}

bool ValidState(std::uint64_t state) noexcept {
  return state == kReady || state == kFinished;
}

// A stamp must refer to this slot, even when it reports a newer overwrite.
bool ValidStamp(std::uint64_t stamp, std::uint64_t expected,
                std::size_t capacity) noexcept {
  if (!stamp) return true;
  const auto sequence = (stamp >> 1) + (stamp & 1);
  return sequence <= kMaxBroadcastSequence &&
         ((sequence - 1) & (capacity - 1)) == ((expected - 1) & (capacity - 1));
}

// Monotonic local attach identities never wrap and require no entropy source.
Status NewSession(BroadcastId& id) noexcept {
  auto next = next_session.load(std::memory_order_relaxed);
  for (;;) {
    if (next == std::numeric_limits<std::uint64_t>::max())
      return Error(ErrorCode::kResourceExhausted);
    if (next_session.compare_exchange_weak(next, next + 1,
                                           std::memory_order_relaxed)) {
      id = {0, next};
      return {};
    }
  }
}
}  // namespace

namespace internal {
AtomicWord& BroadcastSlot(std::byte* slots, std::size_t stride,
                          std::size_t capacity, std::uint64_t sequence,
                          std::size_t word) noexcept {
  const auto index = (sequence - 1) & (capacity - 1);
  return *reinterpret_cast<AtomicWord*>(slots + index * stride +
                                        word * sizeof(AtomicWord));
}
}  // namespace internal

BroadcastQueue::SizeResult BroadcastQueue::RequiredBytes(
    BroadcastConfig config) noexcept {
  BroadcastLayout layout;
  const auto status = MakeLayout(config, {}, layout);
  return {status, status.ok() ? static_cast<std::size_t>(layout.bytes) : 0};
}

Status BroadcastQueue::Initialize(std::span<std::byte> memory,
                                  BroadcastConfig config,
                                  BroadcastId instance_id) noexcept {
  BroadcastLayout layout;
  const auto status = MakeLayout(config, instance_id, layout);
  if (!status.ok()) return status;
  if (instance_id == BroadcastId{} || memory.size() < layout.bytes ||
      reinterpret_cast<std::uintptr_t>(memory.data()) % kAlignment)
    return Error(ErrorCode::kInvalidArgument);
  auto* header = ::new (memory.data()) BroadcastHeader{};
  header->layout = layout;
  auto* slots = memory.data() + layout.slots_offset;
  for (std::size_t i = 0; i < config.capacity; ++i) {
    for (std::size_t word = 0; word < layout.word_count + kSlotMetadataWords;
         ++word) {
      ::new (slots + i * layout.slot_stride + word * sizeof(AtomicWord))
          AtomicWord{0};
    }
  }
  header->state.store(kReady, std::memory_order_release);
  return {};
}

Status BroadcastQueue::Attach(std::span<std::byte> memory) noexcept {
  if (attached()) return Error(ErrorCode::kInvalidState);
  if (memory.size() < sizeof(BroadcastHeader) ||
      reinterpret_cast<std::uintptr_t>(memory.data()) % kAlignment)
    return Error(ErrorCode::kInvalidArgument);
  BroadcastLayout layout;
  std::memcpy(&layout, memory.data(), sizeof(layout));
  BroadcastLayout expected;
  if (layout.capacity > std::numeric_limits<std::size_t>::max() ||
      layout.max_record_size > std::numeric_limits<std::size_t>::max() ||
      !MakeLayout({static_cast<std::size_t>(layout.capacity),
                   static_cast<std::size_t>(layout.max_record_size)},
                  layout.generation, expected)
           .ok() ||
      layout != expected || layout.bytes > memory.size() ||
      layout.generation == BroadcastId{})
    return Error(ErrorCode::kCorrupt);
  auto* header = reinterpret_cast<BroadcastHeader*>(memory.data());
  if (!ValidState(header->state.load(std::memory_order_acquire)) ||
      header->published_sequence.load(std::memory_order_acquire) >
          kMaxBroadcastSequence)
    return Error(ErrorCode::kCorrupt);
  memory_ = memory.first(layout.bytes);
  header_ = header;
  slots_ = memory.data() + layout.slots_offset;
  capacity_ = layout.capacity;
  max_record_size_ = layout.max_record_size;
  word_count_ = layout.word_count;
  slot_stride_ = layout.slot_stride;
  generation_ = layout.generation;
  return {};
}
void BroadcastQueue::Detach() noexcept { *this = {}; }
bool BroadcastQueue::finished() const noexcept {
  return attached() &&
         header_->state.load(std::memory_order_acquire) == kFinished;
}

Status BroadcastWriter::Attach(std::span<std::byte> memory) noexcept {
  const auto status = queue_.Attach(memory);
  if (!status.ok()) return status;
  if (queue_.header_->state.load(std::memory_order_acquire) != kReady) {
    queue_.Detach();
    return Error(ErrorCode::kInvalidState);
  }
  const auto published =
      queue_.header_->published_sequence.load(std::memory_order_acquire);
  if (published > kMaxBroadcastSequence) {
    queue_.Detach();
    return Error(ErrorCode::kCorrupt);
  }
  next_sequence_ = published + 1;
  return {};
}
Status BroadcastWriter::TryWrite(std::span<const std::byte> record,
                                 std::uint64_t* sequence) noexcept {
  if (!queue_.attached()) return Error(ErrorCode::kInvalidState);
  if (queue_.header_->state.load(std::memory_order_acquire) != kReady)
    return Error(ErrorCode::kInvalidState);
  if (record.size() > queue_.max_record_size_ ||
      next_sequence_ > kMaxBroadcastSequence)
    return Error(ErrorCode::kResourceExhausted);
  if (Overlaps(record, queue_.memory_))
    return Error(ErrorCode::kInvalidArgument);
  const auto slot = [&](std::size_t word) -> AtomicWord& {
    return BroadcastSlot(queue_.slots_, queue_.slot_stride_, queue_.capacity_,
                         next_sequence_, word);
  };
  slot(0).store(2 * next_sequence_ - 1, std::memory_order_relaxed);
  // Publish invalidation before any payload/length store. Atomic payload words
  // prevent data races even when readers copy an overwritten record.
  std::atomic_thread_fence(std::memory_order_release);
  slot(1).store(record.size(), std::memory_order_relaxed);
  for (std::size_t i = 0; i < queue_.word_count_; ++i) {
    std::uint64_t word = 0;
    const auto offset = i * sizeof(word);
    if (offset < record.size())
      std::memcpy(&word, record.data() + offset,
                  std::min(sizeof(word), record.size() - offset));
    slot(i + kSlotMetadataWords).store(word, std::memory_order_relaxed);
  }
  slot(0).store(2 * next_sequence_, std::memory_order_release);
  queue_.header_->published_sequence.store(next_sequence_,
                                           std::memory_order_release);
  if (sequence) *sequence = next_sequence_;
  ++next_sequence_;
  return {};
}
Status BroadcastWriter::Finish() noexcept {
  if (!queue_.attached()) return Error(ErrorCode::kInvalidState);
  const auto state = queue_.header_->state.load(std::memory_order_acquire);
  if (!ValidState(state)) return Error(ErrorCode::kCorrupt);
  queue_.header_->state.store(kFinished, std::memory_order_release);
  return {};
}
void BroadcastWriter::Detach() noexcept { queue_.Detach(); }

Status BroadcastReader::Attach(std::span<std::byte> memory,
                               std::span<std::byte> scratch,
                               BroadcastStartPosition start,
                               BroadcastId reader_identity) noexcept {
  auto status = queue_.Attach(memory);
  if (!status.ok()) return status;
  const auto published =
      queue_.header_->published_sequence.load(std::memory_order_acquire);
  if (published > kMaxBroadcastSequence) {
    queue_.Detach();
    return Error(ErrorCode::kCorrupt);
  }
  if (scratch.size() < queue_.max_record_size_ || Overlaps(memory, scratch) ||
      (start.generation && *start.generation != queue_.generation_) ||
      (start.mode != BroadcastStartPosition::Mode::kLive &&
       start.mode != BroadcastStartPosition::Mode::kSequence) ||
      (start.mode == BroadcastStartPosition::Mode::kSequence &&
       (start.sequence == 0 || start.sequence > published + 1))) {
    queue_.Detach();
    return Error(ErrorCode::kInvalidArgument);
  }
  status = NewSession(session_);
  if (!status.ok()) {
    queue_.Detach();
    return status;
  }
  if (reader_identity != BroadcastId{}) session_ = reader_identity;
  scratch_ = scratch.first(queue_.max_record_size_);
  next_sequence_ = start.mode == BroadcastStartPosition::Mode::kLive
                       ? published + 1
                       : start.sequence;
  return {};
}
BroadcastReadResult BroadcastReader::Result(
    BroadcastReadStatus status, std::size_t attempts) const noexcept {
  BroadcastReadResult result;
  result.status = status;
  result.generation = queue_.generation();
  result.sequence = next_sequence_;
  result.attempts = attempts;
  return result;
}
BroadcastReadResult BroadcastReader::Fail(std::size_t attempts) noexcept {
  corrupt_ = true;
  auto result = Result(BroadcastReadStatus::kError, attempts);
  result.error = Error(ErrorCode::kCorrupt);
  return result;
}
BroadcastReadResult BroadcastReader::TryRead(
    BroadcastReadBudget budget) noexcept {
  if (!queue_.attached()) return Result(BroadcastReadStatus::kDetached);
  if (corrupt_) return Fail(0);
  if (pending_gap_) {
    auto result = Result(BroadcastReadStatus::kGap);
    result.gap = pending_gap_;
    return result;
  }
  if (!budget.max_attempts) {
    auto result = Result(BroadcastReadStatus::kError);
    result.error = Error(ErrorCode::kInvalidArgument);
    return result;
  }
  for (std::size_t attempts = 0; attempts < budget.max_attempts;) {
    // Observe Finish before the tail so its final commit is included.
    const auto state = queue_.header_->state.load(std::memory_order_acquire);
    const auto published =
        queue_.header_->published_sequence.load(std::memory_order_acquire);
    if (!ValidState(state) || published > kMaxBroadcastSequence)
      return Fail(attempts);
    if (next_sequence_ > published)
      return Result(state == kFinished ? BroadcastReadStatus::kEnd
                                       : BroadcastReadStatus::kEmpty,
                    attempts);
    const auto slot = [&](std::size_t word) -> AtomicWord& {
      return BroadcastSlot(queue_.slots_, queue_.slot_stride_, queue_.capacity_,
                           next_sequence_, word);
    };
    const auto first = slot(0).load(std::memory_order_acquire);
    ++attempts;
    if (!ValidStamp(first, next_sequence_, queue_.capacity_))
      return Fail(attempts);
    auto observed = (first >> 1) + (first & 1);
    if (observed <= next_sequence_) {
      if (first != 2 * next_sequence_) continue;
      const auto length = slot(1).load(std::memory_order_relaxed);
      // Copy the entire configured slot, never an unvalidated shared length.
      for (std::size_t i = 0; i < queue_.word_count_; ++i) {
        const auto word =
            slot(i + kSlotMetadataWords).load(std::memory_order_relaxed);
        const auto offset = i * sizeof(word);
        std::memcpy(scratch_.data() + offset, &word,
                    std::min(sizeof(word), scratch_.size() - offset));
      }
      // Revalidate after payload loads; pairs with an overwriter's release
      // fence through any atomic word read from that overwriter.
      std::atomic_thread_fence(std::memory_order_acquire);
      const auto last = slot(0).load(std::memory_order_relaxed);
      if (!ValidStamp(last, next_sequence_, queue_.capacity_))
        return Fail(attempts);
      observed = (last >> 1) + (last & 1);
      if (observed <= next_sequence_) {
        if (first != last) continue;
        if (length > queue_.max_record_size_) return Fail(attempts);
        auto result = Result(BroadcastReadStatus::kRecord, attempts);
        result.record = scratch_.first(length);
        ++next_sequence_;
        return result;
      }
    }
    ++gap_id_;
    pending_gap_ =
        BroadcastGapToken{queue_.generation_, gap_id_, next_sequence_,
                          observed - queue_.capacity_ + 1, session_};
    auto result = Result(BroadcastReadStatus::kGap, attempts);
    result.gap = pending_gap_;
    return result;
  }
  return Result(BroadcastReadStatus::kRetry, budget.max_attempts);
}
Status BroadcastReader::Resume(const BroadcastGapToken& token,
                               std::uint64_t& output,
                               BroadcastResumePosition position) noexcept {
  if (!queue_.attached()) return Error(ErrorCode::kInvalidState);
  if (!pending_gap_ || token != *pending_gap_ ||
      token.generation != queue_.generation_)
    return Error(ErrorCode::kInvalidArgument);
  const auto published =
      queue_.header_->published_sequence.load(std::memory_order_acquire);
  if (published > kMaxBroadcastSequence) return Error(ErrorCode::kCorrupt);
  std::uint64_t sequence = 0;
  switch (position.mode) {
    case BroadcastResumePosition::Mode::kCandidate:
      sequence = token.resume_candidate;
      break;
    case BroadcastResumePosition::Mode::kLive:
      sequence = published + 1;
      break;
    case BroadcastResumePosition::Mode::kSequence:
      sequence = position.sequence;
      break;
    default:
      return Error(ErrorCode::kInvalidArgument);
  }
  if (!sequence || sequence > published + 1)
    return Error(ErrorCode::kInvalidArgument);
  next_sequence_ = sequence;
  pending_gap_.reset();
  output = sequence;
  return {};
}
void BroadcastReader::Detach() noexcept {
  queue_.Detach();
  scratch_ = {};
  next_sequence_ = 1;
  gap_id_ = 0;
  session_ = {};
  pending_gap_.reset();
  corrupt_ = false;
}
}  // namespace yquant::base
