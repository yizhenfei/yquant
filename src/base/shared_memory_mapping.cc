#include "base/internal/shared_memory_mapping.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <limits>

namespace yquant::base::internal {
namespace {
constexpr std::size_t kMaxMappingBytes = std::size_t{1} << 30;
Status SystemError(int number = errno) {
  return {ErrorCode::kIoError, {number, std::generic_category()}};
}
Status Corrupt() { return {ErrorCode::kCorrupt, {}}; }
// Darwin shm descriptors may omit a file type; Linux reports a regular file.
bool SharedObjectMode(mode_t mode) {
  if (S_ISREG(mode)) return true;
#if defined(__APPLE__)
  return (mode & S_IFMT) == 0;
#else
  return false;
#endif
}
bool SameObject(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
}
}  // namespace
Status SharedMemoryBytes(std::size_t required, std::size_t& bytes) noexcept {
  const long page = ::sysconf(_SC_PAGESIZE);
  if (page <= 0 || !std::has_single_bit(static_cast<std::size_t>(page)))
    return {ErrorCode::kUnsupported, {}};
  const auto mask = static_cast<std::size_t>(page) - 1;
  if (!required || required > kMaxMappingBytes ||
      required > std::numeric_limits<std::size_t>::max() - mask)
    return {ErrorCode::kInvalidArgument, {}};
  const auto rounded = (required + mask) & ~mask;
  if (rounded > kMaxMappingBytes ||
      rounded > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
    return {ErrorCode::kInvalidArgument, {}};
  bytes = rounded;
  return {};
}
SharedMemoryMapping::~SharedMemoryMapping() { Close(); }
Status SharedMemoryMapping::Create(const std::string& name, std::size_t bytes) {
  if (fd_ >= 0) return {ErrorCode::kInvalidState, {}};
  std::size_t rounded;
  auto status = SharedMemoryBytes(bytes, rounded);
  if (!status.ok()) return status;
  const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) return SystemError();
  fd_ = fd;
  name_ = name;
  owner_pid_ = ::getpid();
  bytes_ = rounded;
  if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0 ||
      ::ftruncate(fd_, static_cast<off_t>(bytes_)) != 0) {
    status = SystemError();
    Close();
    return status;
  }
#if defined(__linux__)
  int allocated;
  do {
    allocated = ::posix_fallocate(fd_, 0, static_cast<off_t>(bytes_));
  } while (allocated == EINTR);
  if (allocated) {
    status = SystemError(allocated);
    Close();
    return status;
  }
#endif
  void* mapped =
      ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapped == MAP_FAILED) {
    status = SystemError();
    Close();
    return status;
  }
  memory_ = static_cast<std::byte*>(mapped);
  return {};
}
Status SharedMemoryMapping::Open(const std::string& name) {
  if (fd_ >= 0) return {ErrorCode::kInvalidState, {}};
  const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0) return SystemError();
  fd_ = fd;
  name_ = name;
  Status status;
  struct stat before{}, after{};
  if (::fcntl(fd_, F_SETFD, FD_CLOEXEC) != 0 || ::fstat(fd_, &before) != 0) {
    status = SystemError();
    Close();
    return status;
  }
  if (!SharedObjectMode(before.st_mode) || before.st_uid != ::geteuid() ||
      (before.st_mode & 0022) || before.st_size <= 0 ||
      static_cast<std::uintmax_t>(before.st_size) > kMaxMappingBytes) {
    Close();
    return Corrupt();
  }
  bytes_ = static_cast<std::size_t>(before.st_size);
  void* mapped =
      ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (mapped == MAP_FAILED) {
    status = SystemError();
    Close();
    return status;
  }
  memory_ = static_cast<std::byte*>(mapped);
  if (::fstat(fd_, &after) != 0) {
    status = SystemError();
    Close();
    return status;
  }
  if (!SameObject(before, after)) {
    Close();
    return {ErrorCode::kInvalidState, {}};
  }
  return {};
}
Status SharedMemoryMapping::VerifyName() const {
  if (fd_ < 0) return {ErrorCode::kInvalidState, {}};
  const int named = ::shm_open(name_.c_str(), O_RDWR, 0);
  if (named < 0) return SystemError();
  struct stat mapped{}, current{};
  const bool checked =
      ::fstat(fd_, &mapped) == 0 && ::fstat(named, &current) == 0;
  const int number = errno;
  ::close(named);
  if (!checked) return SystemError(number);
  return SameObject(mapped, current) ? Status{}
                                     : Status{ErrorCode::kInvalidState, {}};
}
void SharedMemoryMapping::Close() noexcept {
  if (owner_pid_ && owner_pid_ == ::getpid()) ::shm_unlink(name_.c_str());
  if (memory_) ::munmap(memory_, bytes_);
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  memory_ = nullptr;
  bytes_ = 0;
  owner_pid_ = 0;
  name_.clear();
}
}  // namespace yquant::base::internal
