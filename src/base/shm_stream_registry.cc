#include "base/internal/shm_stream_registry.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <string_view>

namespace yquant::base::internal {
namespace {

constexpr std::size_t kMaxShmManifestBytes = 256;
using AtomicWord = std::atomic<std::uint64_t>;

// Retains POSIX errno without allocating diagnostic text.
base::Status MakeError(base::ErrorCode code, int number = 0) {
  return {code, number ? std::error_code(number, std::generic_category())
                       : std::error_code{}};
}

base::Status SystemError(int number = errno) {
  return MakeError(base::ErrorCode::kIoError, number);
}

// This counter is process-private: only the child changes its inherited copy.
// Checking an endpoint's epoch therefore needs no getpid syscall on each event.
AtomicWord process_epoch{0};
pthread_once_t fork_guard_once = PTHREAD_ONCE_INIT;
int fork_guard_registration_error = 0;

void AfterForkChild() noexcept {
  process_epoch.fetch_add(1, std::memory_order_relaxed);
}

void RegisterForkGuard() noexcept {
  fork_guard_registration_error =
      ::pthread_atfork(nullptr, nullptr, AfterForkChild);
}

// Registers exactly once; fork children advance a process-private ownership
// epoch.
Result<void> PrepareForkGuard() {
  const int once_error = ::pthread_once(&fork_guard_once, RegisterForkGuard);
  if (once_error != 0) return SystemError(once_error);
  if (fork_guard_registration_error != 0)
    return SystemError(fork_guard_registration_error);
  return {};
}

// Owns a descriptor until Release; it never explicitly unlocks a flock.
class FileDescriptor {
 public:
  explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
  ~FileDescriptor() {
    if (fd_ >= 0) ::close(fd_);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  int get() const noexcept { return fd_; }
  int Release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

 private:
  int fd_;
};

// Normalizes the discovery root and restricts stream names to one path element.
Result<ShmEndpoint> ValidateEndpointImpl(const ShmEndpoint& endpoint) {
  if (endpoint.stream.empty() || endpoint.stream.size() > 64)
    return MakeError(base::ErrorCode::kInvalidArgument);
  const auto alnum = [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
  };
  if (!alnum(static_cast<unsigned char>(endpoint.stream.front())))
    return MakeError(base::ErrorCode::kInvalidArgument);
  for (unsigned char c : endpoint.stream) {
    if (!alnum(c) && c != '_' && c != '-' && c != '.')
      return MakeError(base::ErrorCode::kInvalidArgument);
  }
  if (endpoint.runtime_directory.empty() ||
      !endpoint.runtime_directory.is_absolute() ||
      endpoint.runtime_directory.native().find('\0') != std::string::npos)
    return MakeError(base::ErrorCode::kInvalidArgument);
  for (const auto& part : endpoint.runtime_directory) {
    if (part == "..") return MakeError(base::ErrorCode::kInvalidArgument);
  }
  auto path = endpoint.runtime_directory.lexically_normal();
  while (path != path.root_path() && path.filename().empty())
    path = path.parent_path();
  return ShmEndpoint{std::move(path), endpoint.stream};
}

// Returns ownership of a secured directory fd, or a status with no open fd.
Result<int> OpenRuntimeDirectory(const ShmEndpoint& endpoint, bool create) {
  bool created = false;
  if (create) {
    std::error_code ec;
    created =
        std::filesystem::create_directories(endpoint.runtime_directory, ec);
    if (ec) return SystemError(ec.value());
  }
  FileDescriptor fd(::open(endpoint.runtime_directory.c_str(),
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (fd.get() < 0) return SystemError();
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0) return SystemError();
  if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
    return MakeError(base::ErrorCode::kInvalidArgument);
  if (created && ::fchmod(fd.get(), 0700) != 0) return SystemError();
  if (!created && (info.st_mode & 0022) != 0)
    return MakeError(base::ErrorCode::kInvalidArgument);
  return fd.Release();
}

std::string Hex(std::uint64_t value, unsigned digits = 16) {
  constexpr char alphabet[] = "0123456789abcdef";
  std::string result(digits, '0');
  for (unsigned i = 0; i != digits; ++i) {
    result[digits - 1 - i] = alphabet[value & 15];
    value >>= 4;
  }
  return result;
}

std::string SharedName(BroadcastId generation) {
  // Darwin limits shm names to 31 bytes. Retain 96 random bits in the name;
  // the full 128-bit generation is checked in the manifest and mapped header.
  return "/yq" + Hex(generation.high, 8) + Hex(generation.low);
}

// Reads a complete identity from POSIX entropy, retrying only interrupted
// reads.
Result<BroadcastId> ReadRandomBroadcastId() {
  FileDescriptor fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return SystemError();
  BroadcastId generation;
  auto* target = reinterpret_cast<std::byte*>(&generation);
  std::size_t remaining = sizeof(generation);
  while (remaining != 0) {
    const ssize_t count = ::read(fd.get(), target, remaining);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return SystemError(count == 0 ? EIO : errno);
    target += count;
    remaining -= static_cast<std::size_t>(count);
  }
  // Zero is reserved for an absent generation in client-side state.
  if (generation == BroadcastId{}) generation.low = 1;
  return generation;
}

bool ParseHex(std::string_view value, std::uint64_t& result) noexcept {
  if (value.size() != 16) return false;
  for (char c : value)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result, 16);
  return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

// Rejects nonregular, oversized or malformed entries before allocating a name.
Result<ShmManifest> ReadShmManifest(int directory,
                                    const std::string& filename) {
  FileDescriptor fd(::openat(directory, filename.c_str(),
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0) return SystemError();
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0) return SystemError();
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0022) != 0 || info.st_size <= 0 ||
      info.st_size > static_cast<off_t>(kMaxShmManifestBytes))
    return MakeError(base::ErrorCode::kCorrupt);
  std::array<char, kMaxShmManifestBytes + 1> bytes{};
  std::size_t used = 0;
  while (used < bytes.size()) {
    const ssize_t count =
        ::read(fd.get(), bytes.data() + used, bytes.size() - used);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) return SystemError();
    if (count == 0) break;
    used += static_cast<std::size_t>(count);
  }
  if (used != static_cast<std::size_t>(info.st_size) ||
      used > kMaxShmManifestBytes)
    return MakeError(base::ErrorCode::kCorrupt);
  const std::string_view text(bytes.data(), used);
  constexpr std::string_view prefix = "yquant-shm-v1\n";
  if (!text.starts_with(prefix) || text.size() <= prefix.size() + 33 ||
      text.back() != '\n')
    return MakeError(base::ErrorCode::kCorrupt);
  const auto body = text.substr(prefix.size());
  ShmManifest manifest;
  if (body[32] != '\n' ||
      !ParseHex(body.substr(0, 16), manifest.generation.high) ||
      !ParseHex(body.substr(16, 16), manifest.generation.low) ||
      manifest.generation == BroadcastId{})
    return MakeError(base::ErrorCode::kCorrupt);
  const auto name = body.substr(33, body.size() - 34);
  if (name != SharedName(manifest.generation))
    return MakeError(base::ErrorCode::kCorrupt);
  manifest.shm_name = std::string(name);
  return manifest;
}

// Writes a private temporary entry then atomically replaces discovery by
// rename. Failure removes the temporary file and leaves existing discovery
// untouched.
Result<void> WriteShmManifest(int directory, const std::string& filename,
                              const ShmManifest& manifest) {
  const std::string temporary = "." + filename + "." +
                                Hex(manifest.generation.high) +
                                Hex(manifest.generation.low);
  const std::string body = "yquant-shm-v1\n" + Hex(manifest.generation.high) +
                           Hex(manifest.generation.low) + "\n" +
                           manifest.shm_name + "\n";
  FileDescriptor fd(
      ::openat(directory, temporary.c_str(),
               O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
  if (fd.get() < 0) return SystemError();
  struct TemporaryEntry {
    int directory;
    const std::string& name;
    bool active{true};
    ~TemporaryEntry() {
      if (active) ::unlinkat(directory, name.c_str(), 0);
    }
  } temporary_entry{directory, temporary};
  std::size_t written = 0;
  while (written != body.size()) {
    const ssize_t count =
        ::write(fd.get(), body.data() + written, body.size() - written);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      const int number = count == 0 ? EIO : errno;
      return SystemError(number);
    }
    written += static_cast<std::size_t>(count);
  }
  if (::renameat(directory, temporary.c_str(), directory, filename.c_str()) !=
      0) {
    const int number = errno;
    return SystemError(number);
  }
  temporary_entry.active = false;
  return {};
}

}  // namespace

Result<ShmEndpoint> ValidateShmEndpoint(const ShmEndpoint& endpoint) {
  return ValidateEndpointImpl(endpoint);
}

Result<BroadcastId> RandomBroadcastId() { return ReadRandomBroadcastId(); }

std::uint64_t MonotonicNs() noexcept {
  struct timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

Result<ShmManifest> Discover(const ShmEndpoint& endpoint) {
  auto validated = ValidateShmEndpoint(endpoint);
  if (!validated) return validated.error();
  auto directory = OpenRuntimeDirectory(*validated, false);
  if (!directory) return directory.error();
  FileDescriptor directory_fd(*directory);
  return ReadShmManifest(directory_fd.get(), validated->stream + ".manifest");
}

ShmStreamRegistry::~ShmStreamRegistry() { Close(); }
Status ShmStreamRegistry::Acquire(const ShmEndpoint& endpoint) {
  if (directory_fd_ >= 0) return MakeError(ErrorCode::kInvalidState);
  auto checked = ValidateShmEndpoint(endpoint);
  if (!checked) return checked.error();
  auto guarded = PrepareForkGuard();
  if (!guarded) return guarded.error();
  auto directory = OpenRuntimeDirectory(*checked, true);
  if (!directory) return directory.error();
  endpoint_ = std::move(*checked);
  directory_fd_ = *directory;
  owner_epoch_ = process_epoch.load(std::memory_order_relaxed);
  manifest_filename_ = endpoint_.stream + ".manifest";
  pending_filename_ = endpoint_.stream + ".pending";
  const auto lock_filename = endpoint_.stream + ".lock";
  lock_fd_ = ::openat(directory_fd_, lock_filename.c_str(),
                      O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  auto fail = [&](Status status) {
    Close();
    return status;
  };
  if (lock_fd_ < 0) return fail(SystemError());
  struct stat info{};
  if (::fstat(lock_fd_, &info) != 0) return fail(SystemError());
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      info.st_nlink != 1 || (info.st_mode & 0022))
    return fail(MakeError(ErrorCode::kInvalidArgument));
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    const int number = errno;
    return fail(number == EWOULDBLOCK || number == EAGAIN
                    ? MakeError(ErrorCode::kInvalidState, number)
                    : SystemError(number));
  }
  for (const auto& filename : {pending_filename_, manifest_filename_}) {
    auto old = ReadShmManifest(directory_fd_, filename);
    if (old) {
      if (::shm_unlink(old->shm_name.c_str()) != 0 && errno != ENOENT)
        return fail(SystemError());
      if (::unlinkat(directory_fd_, filename.c_str(), 0) != 0 &&
          errno != ENOENT)
        return fail(SystemError());
    } else if (old.error().system_error.value() != ENOENT) {
      return fail(old.error());
    }
  }
  return {};
}
Status ShmStreamRegistry::Prepare() {
  if (!owned_by_current_process()) return MakeError(ErrorCode::kInvalidState);
  if (published_) return MakeError(ErrorCode::kInvalidState);
  auto id = RandomBroadcastId();
  if (!id) return id.error();
  ShmManifest manifest{*id, SharedName(*id)};
  auto status = WriteShmManifest(directory_fd_, pending_filename_, manifest);
  if (!status) return status.error();
  manifest_ = std::move(manifest);
  pending_ = true;
  return {};
}
Status ShmStreamRegistry::Publish() {
  if (!owned_by_current_process() || !pending_ || published_)
    return MakeError(ErrorCode::kInvalidState);
  auto status = WriteShmManifest(directory_fd_, manifest_filename_, manifest_);
  if (!status) return status.error();
  published_ = true;
  if (::unlinkat(directory_fd_, pending_filename_.c_str(), 0) != 0 &&
      errno != ENOENT)
    return SystemError();
  pending_ = false;
  return {};
}
void ShmStreamRegistry::Retire() noexcept {
  if (!owned_by_current_process()) return;
  const auto remove_owned = [&](const std::string& filename) {
    auto current = ReadShmManifest(directory_fd_, filename);
    if (current && current->generation == manifest_.generation &&
        current->shm_name == manifest_.shm_name)
      ::unlinkat(directory_fd_, filename.c_str(), 0);
  };
  if (published_) remove_owned(manifest_filename_);
  if (pending_) remove_owned(pending_filename_);
  published_ = false;
  pending_ = false;
}
void ShmStreamRegistry::Close() noexcept {
  Retire();
  // Never LOCK_UN: fork children share the parent's open file description.
  if (lock_fd_ >= 0) ::close(lock_fd_);
  if (directory_fd_ >= 0) ::close(directory_fd_);
  lock_fd_ = -1;
  directory_fd_ = -1;
  pending_ = false;
  published_ = false;
  manifest_ = {};
}
bool ShmStreamRegistry::owned_by_current_process() const noexcept {
  return lock_fd_ >= 0 &&
         owner_epoch_ == process_epoch.load(std::memory_order_relaxed);
}
}  // namespace yquant::base::internal
