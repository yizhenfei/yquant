#ifndef YQUANT_MARKETD_REPLAY_DATA_H_
#define YQUANT_MARKETD_REPLAY_DATA_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <new>
#include <utility>

#include "marketd/file_config.h"
#include "yquant/base/status.h"
#include "yquant/model/quote_tick.h"

namespace yquant::marketd {

// File order is authoritative; offset_ns is a nondecreasing arrival offset.
struct ReplayRecord {
  uint64_t offset_ns = 0;
  model::QuoteTick quote;
};

// Single-owner, preallocated initialization storage. Readers build a temporary
// instance and move it to their output only after validating the complete file.
class ReplayData {
 public:
  ReplayData() noexcept = default;
  ReplayData(ReplayData&& other) noexcept { *this = std::move(other); }
  ReplayData& operator=(ReplayData&& other) noexcept {
    if (this != &other) {
      records_ = std::move(other.records_);
      size_ = std::exchange(other.size_, 0);
      capacity_ = std::exchange(other.capacity_, 0);
    }
    return *this;
  }
  ReplayData(const ReplayData&) = delete;
  ReplayData& operator=(const ReplayData&) = delete;

  // Replaces storage and clears the count on success; failure preserves data.
  [[nodiscard]] base::Status Reserve(size_t capacity) noexcept {
    if (capacity > kMaxReplayRecords) {
      return {base::ErrorCode::kInvalidArgument, {}};
    }
    std::unique_ptr<ReplayRecord[]> records;
    if (capacity != 0) {
      records.reset(new (std::nothrow) ReplayRecord[capacity]);
      if (!records) {
        return {base::ErrorCode::kResourceExhausted, {}};
      }
    }
    records_ = std::move(records);
    size_ = 0;
    capacity_ = capacity;
    return {};
  }

  // Appends without allocation; a full buffer leaves existing records intact.
  [[nodiscard]] base::Status Append(const ReplayRecord& record) noexcept {
    if (size_ == capacity_) {
      return {base::ErrorCode::kResourceExhausted, {}};
    }
    records_[size_++] = record;
    return {};
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }
  ReplayRecord& operator[](size_t index) noexcept { return records_[index]; }
  const ReplayRecord& operator[](size_t index) const noexcept {
    return records_[index];
  }
  void Reset() noexcept {
    records_.reset();
    size_ = capacity_ = 0;
  }

 private:
  std::unique_ptr<ReplayRecord[]> records_;
  size_t size_ = 0;
  size_t capacity_ = 0;
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_REPLAY_DATA_H_
