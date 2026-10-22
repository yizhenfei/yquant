#ifndef YQUANT_BASE_INTERNAL_SHARED_MEMORY_MAPPING_H_
#define YQUANT_BASE_INTERNAL_SHARED_MEMORY_MAPPING_H_

#include <span>
#include <string>

#include "yquant/base/status.h"

namespace yquant::base::internal {
// POSIX mapping owner; has no knowledge of queue slots or stream discovery.
// All calls are serialized. Destructor unmaps and unlinks only names created
// by this process; inherited fork copies only drop local references.
class SharedMemoryMapping final {
 public:
  SharedMemoryMapping() = default;
  ~SharedMemoryMapping();
  SharedMemoryMapping(const SharedMemoryMapping&) = delete;
  SharedMemoryMapping& operator=(const SharedMemoryMapping&) = delete;
  // Create reserves backing storage before touching pages. Both operations
  // require closed state; failure stays closed and cleans partially made names.
  [[nodiscard]] Status Create(const std::string& name, std::size_t bytes);
  [[nodiscard]] Status Open(const std::string& name);
  // Reopens the name and checks inode/size against the mapped descriptor.
  [[nodiscard]] Status VerifyName() const;
  // Idempotent; only a creator in its owning process can unlink the name.
  void Close() noexcept;
  std::span<std::byte> memory() const noexcept { return {memory_, bytes_}; }

 private:
  std::string name_;
  int fd_ = -1;
  std::byte* memory_ = nullptr;
  std::size_t bytes_ = 0;
  int owner_pid_ = 0;
};
// Shared-memory resource budget and page rounding belong to the mapping layer.
// Failure preserves bytes; logical queue size is computed separately.
Status SharedMemoryBytes(std::size_t required, std::size_t& bytes) noexcept;
}  // namespace yquant::base::internal
#endif  // YQUANT_BASE_INTERNAL_SHARED_MEMORY_MAPPING_H_
