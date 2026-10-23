#ifndef YQUANT_BASE_INTERNAL_SHM_STREAM_REGISTRY_H_
#define YQUANT_BASE_INTERNAL_SHM_STREAM_REGISTRY_H_

#include <stdint.h>

#include <string>

#include "base/internal/result.h"
#include "yquant/base/shm_broadcast.h"

namespace yquant::base::internal {
struct ShmManifest {
  BroadcastId generation;
  std::string shm_name;
};
// No resources are created by validation/discovery; failures carry errno.
Result<ShmEndpoint> ValidateShmEndpoint(const ShmEndpoint& endpoint);
Result<ShmManifest> Discover(const ShmEndpoint& endpoint);
Result<BroadcastId> RandomBroadcastId();
uint64_t MonotonicNs() noexcept;

// Owns stream discovery and the stable writer lock, independent of mappings
// and queue layouts. Single-thread-owned; inherited copies cannot retire the
// parent's entries or release its flock explicitly.
class ShmStreamRegistry final {
 public:
  ShmStreamRegistry() = default;
  ~ShmStreamRegistry();
  ShmStreamRegistry(const ShmStreamRegistry&) = delete;
  ShmStreamRegistry& operator=(const ShmStreamRegistry&) = delete;
  // Acquire exclusive writer ownership and retire previous abandoned names.
  // Failure releases descriptors and stays closed.
  [[nodiscard]] Status Acquire(const ShmEndpoint& endpoint);
  // Persist the intended unique mapping name before its creation. A caller
  // encountering EEXIST may retry Prepare; no live colliding name is unlinked.
  [[nodiscard]] Status Prepare();
  // Publish discovery only after the mapped queue and writer are ready.
  [[nodiscard]] Status Publish();
  // Remove only owned discovery/pending entries; retain the writer lock.
  void Retire() noexcept;
  void Close() noexcept;
  bool owned_by_current_process() const noexcept;
  const ShmManifest& manifest() const noexcept { return manifest_; }

 private:
  ShmEndpoint endpoint_;
  ShmManifest manifest_;
  std::string manifest_filename_;
  std::string pending_filename_;
  int directory_fd_ = -1;
  int lock_fd_ = -1;
  uint64_t owner_epoch_ = 0;
  bool pending_ = false;
  bool published_ = false;
};
}  // namespace yquant::base::internal
#endif  // YQUANT_BASE_INTERNAL_SHM_STREAM_REGISTRY_H_
