#ifndef YQUANT_BASE_SHM_BROADCAST_H_
#define YQUANT_BASE_SHM_BROADCAST_H_

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

#include "yquant/base/broadcast_queue.h"

namespace yquant::base {
// Absolute, user-owned directory not writable by group/others; stream is one
// safe path component. Neither field is part of the queue's memory layout.
struct ShmEndpoint {
  std::filesystem::path runtime_directory;
  std::string stream;
};
struct ShmBroadcastConfig {
  ShmEndpoint endpoint;
  BroadcastConfig queue;
};

// Cold-path endpoint and allocation validation, without creating resources.
[[nodiscard]] Status ValidateShmBroadcastConfig(
    const ShmBroadcastConfig& config) noexcept;

enum class ShmWriterHealth { kReady, kStale, kStopped, kUnknown };
enum class ShmRefreshStatus { kCurrent, kStreamChanged, kMissing };
struct ShmRefreshResult {
  ShmRefreshStatus status = ShmRefreshStatus::kCurrent;
  ShmWriterHealth health = ShmWriterHealth::kUnknown;
  std::optional<BroadcastId> discovered_generation;
};

// Stream replacement is a discovery-layer outcome, separate from queue reads.
struct ShmBroadcastReadResult {
  bool stream_changed = false;
  BroadcastReadResult read;
};

// Owns a registered shared-memory stream and a non-owning queue write endpoint.
// Single-thread-owned; Create may allocate/perform I/O. TryWrite does neither.
// A fork child may only Close its inherited writer; it cannot stop the parent.
class ShmBroadcastWriter final {
 public:
  ShmBroadcastWriter() noexcept;
  ShmBroadcastWriter(ShmBroadcastWriter&&) noexcept;
  ShmBroadcastWriter& operator=(ShmBroadcastWriter&&) noexcept;
  ~ShmBroadcastWriter();
  ShmBroadcastWriter(const ShmBroadcastWriter&) = delete;
  ShmBroadcastWriter& operator=(const ShmBroadcastWriter&) = delete;
  // Requires a closed object. Failure releases owned resources and stays
  // closed.
  [[nodiscard]] Status Create(const ShmBroadcastConfig& config);
  // Copies bytes before returning; failure preserves optional sequence output.
  [[nodiscard]] Status TryWrite(std::span<const std::byte> record,
                                std::uint64_t* sequence = nullptr) noexcept;
  // Cold-path heartbeat update, also required while no records are published.
  [[nodiscard]] Status Tick() noexcept;
  BroadcastId generation() const noexcept;
  std::size_t capacity() const noexcept;
  std::size_t max_record_size() const noexcept;
  // Idempotent: finish queue, retire discovery/name, unmap, release writer
  // lock.
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Single-thread-owned mapping and queue reader. Scratch is caller-owned until
// Close. Result bytes remain valid only until the next TryRead or Close.
class ShmBroadcastReader final {
 public:
  ShmBroadcastReader() noexcept;
  ShmBroadcastReader(ShmBroadcastReader&&) noexcept;
  ShmBroadcastReader& operator=(ShmBroadcastReader&&) noexcept;
  ~ShmBroadcastReader();
  ShmBroadcastReader(const ShmBroadcastReader&) = delete;
  ShmBroadcastReader& operator=(const ShmBroadcastReader&) = delete;
  // Requires closed object. Failure stays closed and does not retain scratch.
  [[nodiscard]] Status Open(const ShmEndpoint& endpoint,
                            std::span<std::byte> scratch,
                            BroadcastStartPosition start = {});
  // No allocation/I/O. A replacement discovered by Refresh is sticky and
  // prevents further reads of the old generation.
  [[nodiscard]] ShmBroadcastReadResult TryRead(
      BroadcastReadBudget budget = {}) noexcept;
  // Failure preserves cursor, pending gap and output; replacement invalidates
  // all old tokens. No automatic reconnect or loss skipping.
  [[nodiscard]] Status Resume(const BroadcastGapToken& token,
                              std::uint64_t& sequence,
                              BroadcastResumePosition position = {}) noexcept;
  // Cold-path discovery and heartbeat check. Failure preserves output.
  [[nodiscard]] Status Refresh(
      ShmRefreshResult& result,
      std::chrono::nanoseconds stale_after = std::chrono::seconds(3));
  BroadcastId generation() const noexcept;
  std::uint64_t next_sequence() const noexcept;
  std::size_t max_record_size() const noexcept;
  void Close() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace yquant::base
#endif  // YQUANT_BASE_SHM_BROADCAST_H_
