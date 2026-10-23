#include "yquant/base/broadcast_queue.h"

#include <stddef.h>
#include <stdint.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <thread>

#include "base/internal/broadcast_layout.h"
#include "gtest/gtest.h"

namespace yquant::base {
namespace {
using internal::AtomicWord;
using internal::BroadcastHeader;
using internal::BroadcastSlot;
using internal::kMaxBroadcastSequence;
using Read = BroadcastReadStatus;

class BroadcastQueueTest : public ::testing::Test {
 protected:
  void SetUp() override { Initialize({4, 32}); }
  void Initialize(BroadcastConfig config) {
    ASSERT_TRUE(BroadcastQueue::Initialize(memory_, config, {1, 2}).ok());
  }
  BroadcastHeader& header() {
    return *reinterpret_cast<BroadcastHeader*>(memory_.data());
  }
  AtomicWord& slot(uint64_t sequence, size_t word = 0) {
    const auto& layout = header().layout;
    return BroadcastSlot(memory_.data() + layout.slots_offset,
                         layout.slot_stride, layout.capacity, sequence, word);
  }
  alignas(BroadcastQueue::required_alignment())
      std::array<std::byte, 65536> memory_{};
};

TEST_F(BroadcastQueueTest, InitializationChecksSizesAndDoesNotMutateOnFailure) {
  const auto before = memory_;
  EXPECT_FALSE(BroadcastQueue::Initialize(memory_, {3, 32}, {1, 2}).ok());
  EXPECT_FALSE(BroadcastQueue::Initialize(memory_, {4, 32}, {}).ok());
  EXPECT_FALSE(
      BroadcastQueue::Initialize(std::span(memory_).subspan(1), {4, 32}, {1, 2})
          .ok());
  EXPECT_FALSE(
      BroadcastQueue::Initialize(std::span(memory_).first(8), {4, 32}, {1, 2})
          .ok());
  EXPECT_EQ(memory_, before);
  EXPECT_FALSE(BroadcastQueue::RequiredBytes({0, 32}).status.ok());
  EXPECT_FALSE(
      BroadcastQueue::RequiredBytes({4, std::numeric_limits<size_t>::max()})
          .status.ok());
  EXPECT_FALSE(
      BroadcastQueue::RequiredBytes({size_t{1} << 63, 32}).status.ok());
}

TEST_F(BroadcastQueueTest, RejectsUntrustedLayoutBeforeAccessingAtomics) {
  const auto original = header().layout;
  const auto rejected = [&] {
    BroadcastQueue view;
    EXPECT_EQ(view.Attach(memory_).code, ErrorCode::kCorrupt);
    EXPECT_FALSE(view.attached());
    header().layout = original;
  };
  header().layout.abi ^= 1;
  rejected();
  header().layout.bytes += 1;
  rejected();
  header().layout.slot_stride = ~uint64_t{};
  rejected();
  header().layout.slots_offset = 1;
  rejected();
  header().layout.capacity = 3;
  rejected();
  header().layout.word_count += 1;
  rejected();
  header().layout.max_record_size = ~uint64_t{};
  rejected();
  header().layout.generation = {};
  rejected();
  BroadcastQueue view;
  EXPECT_FALSE(
      view.Attach(std::span(memory_).first(sizeof(BroadcastHeader))).ok());
  EXPECT_TRUE(view.Attach(memory_).ok());
  EXPECT_EQ(view.capacity(), 4U);
  EXPECT_EQ(view.max_record_size(), 32U);
  EXPECT_FALSE(view.Attach(memory_).ok());
}

TEST_F(BroadcastQueueTest,
       IndependentReadersLiveStartAndVariableLengthRecords) {
  BroadcastWriter writer;
  ASSERT_TRUE(writer.Attach(memory_).ok());
  const std::byte bytes[] = {std::byte{1}, std::byte{2}, std::byte{3}};
  uint64_t sequence = 99;
  ASSERT_TRUE(writer.TryWrite(bytes, &sequence).ok());
  EXPECT_EQ(sequence, 1U);
  BroadcastReader first, second, live;
  std::array<std::byte, 32> a{}, b{}, c{};
  ASSERT_TRUE(first.Attach(memory_, a, BroadcastStartPosition::At(1)).ok());
  ASSERT_TRUE(second.Attach(memory_, b, BroadcastStartPosition::At(1)).ok());
  ASSERT_TRUE(live.Attach(memory_, c).ok());
  EXPECT_EQ(live.next_sequence(), 2U);
  EXPECT_EQ(live.TryRead().status, Read::kEmpty);
  auto result = first.TryRead();
  ASSERT_EQ(result.status, Read::kRecord);
  EXPECT_EQ(result.record.size(), 3U);
  EXPECT_TRUE(std::equal(result.record.begin(), result.record.end(), bytes));
  EXPECT_EQ(second.TryRead().sequence, 1U);
  ASSERT_TRUE(writer.TryWrite({}).ok());
  result = live.TryRead();
  EXPECT_EQ(result.status, Read::kRecord);
  EXPECT_TRUE(result.record.empty());
  EXPECT_EQ(first.TryRead().sequence, 2U);
  EXPECT_EQ(second.TryRead().sequence, 2U);
}

TEST_F(BroadcastQueueTest, NoMarketPayloadLimitAndUnalignedRecordLength) {
  Initialize({2, 8193});
  BroadcastWriter writer;
  BroadcastReader reader;
  std::array<std::byte, 8193> record, scratch;
  record.fill(std::byte{0x7d});
  ASSERT_TRUE(writer.Attach(memory_).ok());
  ASSERT_TRUE(reader.Attach(memory_, scratch).ok());
  ASSERT_TRUE(writer.TryWrite(record).ok());
  auto read = reader.TryRead();
  ASSERT_EQ(read.status, Read::kRecord);
  ASSERT_EQ(read.record.size(), 8193U);
  EXPECT_TRUE(
      std::equal(read.record.begin(), read.record.end(), record.begin()));
}

TEST_F(BroadcastQueueTest, ZeroMaximumRecordSize) {
  Initialize({1, 0});
  BroadcastWriter writer;
  BroadcastReader reader;
  ASSERT_TRUE(writer.Attach(memory_).ok());
  ASSERT_TRUE(reader.Attach(memory_, {}).ok());
  ASSERT_TRUE(writer.TryWrite({}).ok());
  EXPECT_EQ(reader.TryRead().status, Read::kRecord);
  const std::byte byte{};
  EXPECT_FALSE(writer.TryWrite(std::span(&byte, 1)).ok());
}

TEST_F(BroadcastQueueTest, AttachAndWriteFailuresPreserveState) {
  BroadcastReader reader;
  std::array<std::byte, 32> scratch{};
  EXPECT_FALSE(reader.Attach(memory_, std::span(scratch).first(31)).ok());
  EXPECT_FALSE(reader.Attach(memory_, std::span(memory_).first(32)).ok());
  EXPECT_FALSE(
      reader.Attach(memory_, scratch, BroadcastStartPosition::At({9, 9}, 1))
          .ok());
  EXPECT_FALSE(
      reader.Attach(memory_, scratch, BroadcastStartPosition::At(0)).ok());
  EXPECT_FALSE(
      reader.Attach(memory_, scratch, BroadcastStartPosition::At(2)).ok());
  ASSERT_TRUE(reader.Attach(memory_, scratch).ok());
  EXPECT_EQ(reader.TryRead({0}).error.code, ErrorCode::kInvalidArgument);
  EXPECT_EQ(reader.next_sequence(), 1U);
  BroadcastWriter writer;
  EXPECT_FALSE(writer.TryWrite({}).ok());
  ASSERT_TRUE(writer.Attach(memory_).ok());
  uint64_t sequence = 99;
  EXPECT_FALSE(writer.TryWrite(std::array<std::byte, 33>{}, &sequence).ok());
  EXPECT_FALSE(writer.TryWrite(std::span(memory_).first(1), &sequence).ok());
  EXPECT_EQ(sequence, 99U);
  ASSERT_TRUE(writer.TryWrite({}, &sequence).ok());
  EXPECT_EQ(sequence, 1U);
}

TEST_F(BroadcastQueueTest, FinishDrainsAndDetachTransfersWriterOwnership) {
  BroadcastWriter writer;
  ASSERT_TRUE(writer.Attach(memory_).ok());
  ASSERT_TRUE(writer.TryWrite({}).ok());
  writer.Detach();
  ASSERT_TRUE(writer.Attach(memory_).ok());
  uint64_t sequence = 0;
  ASSERT_TRUE(writer.TryWrite({}, &sequence).ok());
  EXPECT_EQ(sequence, 2U);
  ASSERT_TRUE(writer.Finish().ok());
  ASSERT_TRUE(writer.Finish().ok());
  EXPECT_FALSE(writer.TryWrite({}).ok());
  writer.Detach();
  EXPECT_FALSE(writer.Attach(memory_).ok());
  BroadcastReader reader;
  std::array<std::byte, 32> scratch{};
  ASSERT_TRUE(
      reader.Attach(memory_, scratch, BroadcastStartPosition::At(1)).ok());
  EXPECT_EQ(reader.TryRead().sequence, 1U);
  EXPECT_EQ(reader.TryRead().sequence, 2U);
  EXPECT_EQ(reader.TryRead().status, Read::kEnd);
  reader.Detach();
  EXPECT_EQ(reader.TryRead().status, Read::kDetached);
}

TEST_F(BroadcastQueueTest, GapIsStickyAndTokensBelongToOneAttachSession) {
  BroadcastWriter writer;
  BroadcastReader reader, other;
  std::array<std::byte, 32> a{}, b{};
  ASSERT_TRUE(writer.Attach(memory_).ok());
  ASSERT_TRUE(reader.Attach(memory_, a).ok());
  ASSERT_TRUE(other.Attach(memory_, b).ok());
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(writer.TryWrite({}).ok());
  }
  const auto gap = reader.TryRead();
  ASSERT_EQ(gap.status, Read::kGap);
  ASSERT_TRUE(gap.gap);
  EXPECT_EQ(gap.gap->expected_sequence, 1U);
  EXPECT_EQ(gap.gap->resume_candidate, 2U);
  EXPECT_EQ(reader.TryRead().gap, gap.gap);
  EXPECT_EQ(other.TryRead().status, Read::kGap);
  uint64_t sequence = 99;
  EXPECT_FALSE(other.Resume(*gap.gap, sequence).ok());
  EXPECT_FALSE(
      reader.Resume(*gap.gap, sequence, BroadcastResumePosition::At(0)).ok());
  EXPECT_EQ(sequence, 99U);
  ASSERT_TRUE(
      reader.Resume(*gap.gap, sequence, BroadcastResumePosition::At(5)).ok());
  EXPECT_EQ(reader.TryRead().sequence, 5U);
  EXPECT_FALSE(reader.Resume(*gap.gap, sequence).ok());
  other.Detach();
  ASSERT_TRUE(other.Attach(memory_, b, BroadcastStartPosition::At(1)).ok());
  ASSERT_TRUE(other.TryRead().gap);
  EXPECT_FALSE(other.Resume(*gap.gap, sequence).ok());
}

TEST_F(BroadcastQueueTest, BoundedRetryAndPrecommitOverwrite) {
  BroadcastReader reader;
  std::array<std::byte, 32> scratch{};
  ASSERT_TRUE(reader.Attach(memory_, scratch).ok());
  header().published_sequence.store(1, std::memory_order_release);
  slot(1).store(1, std::memory_order_release);
  auto read = reader.TryRead({3});
  EXPECT_EQ(read.status, Read::kRetry);
  EXPECT_EQ(read.attempts, 3U);
  header().published_sequence.store(4, std::memory_order_release);
  slot(5).store(9, std::memory_order_release);
  read = reader.TryRead();
  ASSERT_EQ(read.status, Read::kGap);
  EXPECT_EQ(read.gap->resume_candidate, 2U);
}

TEST_F(BroadcastQueueTest, CorruptLengthAndStampAreSticky) {
  BroadcastWriter writer;
  BroadcastReader reader;
  std::array<std::byte, 32> scratch{};
  ASSERT_TRUE(writer.Attach(memory_).ok());
  ASSERT_TRUE(reader.Attach(memory_, scratch).ok());
  ASSERT_TRUE(writer.TryWrite({}).ok());
  slot(1, 1).store(33, std::memory_order_relaxed);
  EXPECT_EQ(reader.TryRead().error.code, ErrorCode::kCorrupt);
  slot(1, 1).store(0, std::memory_order_relaxed);
  EXPECT_EQ(reader.TryRead().error.code, ErrorCode::kCorrupt);
  EXPECT_EQ(reader.next_sequence(), 1U);
  reader.Detach();
  ASSERT_TRUE(
      reader.Attach(memory_, scratch, BroadcastStartPosition::At(1)).ok());
  slot(1).store(4, std::memory_order_release);  // Sequence 2 belongs to slot 2.
  EXPECT_EQ(reader.TryRead().error.code, ErrorCode::kCorrupt);
}

TEST_F(BroadcastQueueTest, SequenceExhaustionDoesNotWrap) {
  header().published_sequence.store(kMaxBroadcastSequence - 1);
  BroadcastWriter writer;
  ASSERT_TRUE(writer.Attach(memory_).ok());
  uint64_t sequence = 0;
  ASSERT_TRUE(writer.TryWrite({}, &sequence).ok());
  EXPECT_EQ(sequence, (uint64_t{1} << 63) - 1);
  EXPECT_EQ(writer.TryWrite({}).code, ErrorCode::kResourceExhausted);
  EXPECT_EQ(header().published_sequence.load(), sequence);
}

TEST_F(BroadcastQueueTest, ConcurrentOverwriteNeverDeliversMixedRecord) {
  Initialize({8, 257});
  BroadcastWriter writer;
  ASSERT_TRUE(writer.Attach(memory_).ok());
  std::atomic<bool> valid{true};
  std::atomic<unsigned> ready{0};
  std::atomic<size_t> received{0};
  const auto consume = [&] {
    BroadcastReader reader;
    std::array<std::byte, 257> scratch{};
    if (!reader.Attach(memory_, scratch, BroadcastStartPosition::At(1)).ok()) {
      valid = false;
      ++ready;
      return;
    }
    ++ready;
    for (;;) {
      auto result = reader.TryRead({16});
      if (result.status == Read::kRecord) {
        for (size_t i = 0; i < result.record.size(); ++i) {
          if (result.record[i] != std::byte((result.sequence + i) & 255)) {
            valid = false;
          }
        }
        if (result.record.size() != 1 + result.sequence % 257) {
          valid = false;
        }
        ++received;
      } else if (result.status == Read::kGap) {
        uint64_t sequence = 0;
        if (!reader.Resume(*result.gap, sequence).ok()) {
          valid = false;
          return;
        }
      } else if (result.status == Read::kEnd) {
        return;
      } else if (result.status != Read::kRetry &&
                 result.status != Read::kEmpty) {
        valid = false;
        return;
      }
    }
  };
  std::thread first(consume), second(consume);
  while (ready.load() != 2) {
    std::this_thread::yield();
  }
  std::array<std::byte, 257> record;
  for (uint64_t sequence = 1; sequence <= 20000; ++sequence) {
    const auto length = 1 + sequence % record.size();
    for (size_t i = 0; i < length; ++i) {
      record[i] = std::byte((sequence + i) & 255);
    }
    ASSERT_TRUE(writer.TryWrite(std::span(record).first(length)).ok());
  }
  EXPECT_TRUE(writer.Finish().ok());
  first.join();
  second.join();
  EXPECT_TRUE(valid.load());
  EXPECT_GT(received.load(), 0U);
}
}  // namespace
}  // namespace yquant::base
