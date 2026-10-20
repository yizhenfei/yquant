#include "md/internal/shm_segment.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>

namespace yquant::md::internal {
namespace {

constexpr std::size_t kMaxSegmentBytes = std::size_t{1} << 30;
constexpr std::size_t kMaxManifestBytes = 256;
constexpr unsigned kDiscoveryAttempts = 4;
constexpr std::uint64_t kEndianTag = std::endian::native == std::endian::little
                                         ? 0x4c4954544c450001ULL
                                         : 0x424947454e440001ULL;

static_assert(std::is_standard_layout_v<Bootstrap>);
static_assert(std::is_standard_layout_v<SharedHeader>);
static_assert(alignof(AtomicWord) <= kCacheAlignment);
static_assert(offsetof(SharedHeader, bootstrap) == 0);
static_assert(offsetof(SharedHeader, state) % alignof(AtomicWord) == 0);

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

// Checked arithmetic leaves out unchanged when the result would overflow.
bool Add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (b > std::numeric_limits<std::size_t>::max() - a) return false;
  out = a + b;
  return true;
}

bool Multiply(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return false;
  out = a * b;
  return true;
}

// alignment must be a nonzero power of two; out is unchanged on overflow.
bool AlignUp(std::size_t value, std::size_t alignment,
             std::size_t& out) noexcept {
  std::size_t sum;
  if (!Add(value, alignment - 1, sum)) return false;
  out = sum & ~(alignment - 1);
  return true;
}

constexpr std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept {
  for (unsigned i = 0; i != 8; ++i) {
    hash ^= (value >> (8 * i)) & 0xff;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// Fingerprints library and layout details before a peer touches any atomic.
constexpr std::uint64_t AtomicAbi() noexcept {
  std::uint64_t hash = 14695981039346656037ULL;
#if defined(_LIBCPP_VERSION)
  hash = Mix(hash, 1);
  hash = Mix(hash, _LIBCPP_VERSION);
#if defined(_LIBCPP_ABI_VERSION)
  hash = Mix(hash, _LIBCPP_ABI_VERSION);
#endif
#elif defined(__GLIBCXX__)
  hash = Mix(hash, 2);
  hash = Mix(hash, __GLIBCXX__);
#if defined(_GLIBCXX_USE_CXX11_ABI)
  hash = Mix(hash, _GLIBCXX_USE_CXX11_ABI);
#endif
#else
  hash = Mix(hash, 0);
#endif
  hash = Mix(hash, sizeof(AtomicWord));
  hash = Mix(hash, alignof(AtomicWord));
  hash = Mix(hash, sizeof(Bootstrap));
  hash = Mix(hash, sizeof(SharedHeader));
  hash = Mix(hash, alignof(SharedHeader));
  hash = Mix(hash, offsetof(SharedHeader, state));
  hash = Mix(hash, offsetof(SharedHeader, published_sequence));
  return Mix(hash, offsetof(SharedHeader, heartbeat_ns));
}

// Recomputes every offset from bounded inputs; no untrusted offset is reused.
Result<Bootstrap> MakeLayout(std::size_t capacity, std::size_t payload_size,
                             Generation generation) {
  if (capacity == 0 || !std::has_single_bit(capacity) ||
      payload_size > kMaxEventPayloadSize)
    return MakeError(base::ErrorCode::kInvalidArgument);
  const long system_page_size = ::sysconf(_SC_PAGESIZE);
  if (system_page_size <= 0 ||
      !std::has_single_bit(static_cast<std::size_t>(system_page_size)))
    return MakeError(base::ErrorCode::kUnsupported);
  std::size_t bytes, rounded, words, atomic_count, slot_bytes, stride, offset,
      total;
  if (!Add(kWireHeaderSize, payload_size, bytes) ||
      !Add(bytes, sizeof(std::uint64_t) - 1, rounded))
    return MakeError(base::ErrorCode::kInvalidArgument);
  words = rounded / sizeof(std::uint64_t);
  if (!Add(words, 1, atomic_count) ||
      !Multiply(atomic_count, sizeof(AtomicWord), slot_bytes) ||
      !AlignUp(slot_bytes, kCacheAlignment, stride) ||
      !AlignUp(sizeof(SharedHeader), kCacheAlignment, offset) ||
      !Multiply(capacity, stride, bytes) || !Add(offset, bytes, total) ||
      !AlignUp(total, static_cast<std::size_t>(system_page_size), total) ||
      total > kMaxSegmentBytes ||
      total > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
    return MakeError(base::ErrorCode::kInvalidArgument);
  return Bootstrap{kSegmentMagic,
                   kLayoutVersion,
                   kEventSchemaVersion,
                   AtomicAbi(),
                   kEndianTag,
                   sizeof(AtomicWord),
                   alignof(AtomicWord),
                   sizeof(SharedHeader),
                   total,
                   capacity,
                   payload_size,
                   words,
                   stride,
                   offset,
                   generation};
}

// Normalizes the discovery root and restricts stream names to one path element.
Result<Endpoint> ValidateEndpoint(const Endpoint& endpoint) {
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
  return Endpoint{std::move(path), endpoint.stream};
}

// Returns ownership of a secured directory fd, or a status with no open fd.
Result<int> OpenRuntimeDirectory(const Endpoint& endpoint, bool create) {
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

std::string SharedName(Generation generation) {
  // Darwin limits shm names to 31 bytes. Retain 96 random bits in the name;
  // the full 128-bit generation is checked in the manifest and mapped header.
  return "/yq" + Hex(generation.high, 8) + Hex(generation.low);
}

// Reads a complete identity from POSIX entropy, retrying only interrupted
// reads.
Result<Generation> ReadRandomGeneration() {
  FileDescriptor fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
  if (fd.get() < 0) return SystemError();
  Generation generation;
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
  if (generation == Generation{}) generation.low = 1;
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
Result<Manifest> ReadManifest(int directory, const std::string& filename) {
  FileDescriptor fd(::openat(directory, filename.c_str(),
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0) return SystemError();
  struct stat info{};
  if (::fstat(fd.get(), &info) != 0) return SystemError();
  if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
      (info.st_mode & 0022) != 0 || info.st_size <= 0 ||
      info.st_size > static_cast<off_t>(kMaxManifestBytes))
    return MakeError(base::ErrorCode::kCorrupt);
  std::array<char, kMaxManifestBytes + 1> bytes{};
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
      used > kMaxManifestBytes)
    return MakeError(base::ErrorCode::kCorrupt);
  const std::string_view text(bytes.data(), used);
  constexpr std::string_view prefix = "yquant-shm-v1\n";
  if (!text.starts_with(prefix) || text.size() <= prefix.size() + 33 ||
      text.back() != '\n')
    return MakeError(base::ErrorCode::kCorrupt);
  const auto body = text.substr(prefix.size());
  Manifest manifest;
  if (body[32] != '\n' ||
      !ParseHex(body.substr(0, 16), manifest.generation.high) ||
      !ParseHex(body.substr(16, 16), manifest.generation.low) ||
      manifest.generation == Generation{})
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
Result<void> WriteManifest(int directory, const std::string& filename,
                           const Manifest& manifest) {
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

Result<void> ValidateBootstrap(const Bootstrap& boot, std::size_t mapped_size) {
  // Only fixed-width, non-atomic bytes have been inspected at this point.
  if (boot.magic != kSegmentMagic || boot.version != kLayoutVersion ||
      boot.schema != kEventSchemaVersion || boot.abi != AtomicAbi() ||
      boot.endian != kEndianTag || boot.atomic_size != sizeof(AtomicWord) ||
      boot.atomic_alignment != alignof(AtomicWord) ||
      boot.header_size != sizeof(SharedHeader) ||
      mapped_size < sizeof(SharedHeader) ||
      offsetof(SharedHeader, state) > mapped_size - sizeof(AtomicWord))
    return MakeError(base::ErrorCode::kCorrupt);
  return {};
}

// Matches stored dimensions against a freshly calculated, bounded layout.
Result<void> ValidateLayout(const Bootstrap& boot, std::size_t mapped_size,
                            Generation expected_generation) {
  if (boot.capacity > std::numeric_limits<std::size_t>::max() ||
      boot.max_payload_size > std::numeric_limits<std::size_t>::max())
    return MakeError(base::ErrorCode::kCorrupt);
  auto expected = MakeLayout(static_cast<std::size_t>(boot.capacity),
                             static_cast<std::size_t>(boot.max_payload_size),
                             expected_generation);
  if (!expected || boot.generation != expected_generation ||
      boot.segment_size != mapped_size ||
      boot.segment_size != expected->segment_size ||
      boot.header_size != expected->header_size ||
      boot.word_count != expected->word_count ||
      boot.slot_stride != expected->slot_stride ||
      boot.slots_offset != expected->slots_offset)
    return MakeError(base::ErrorCode::kCorrupt);
  return {};
}

bool SameObject(const struct stat& a, const struct stat& b) noexcept {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
}

bool SharedObjectMode(mode_t mode) noexcept {
  if (S_ISREG(mode)) return true;
#if defined(__APPLE__)
  // Darwin's POSIX shm implementation reports permission bits without an
  // S_IFREG type. The descriptor was obtained through shm_open, not open.
  return (mode & S_IFMT) == 0;
#else
  return false;
#endif
}

}  // namespace

struct Segment::Impl {
  Endpoint endpoint;
  Bootstrap layout;
  std::string manifest_filename;
  std::string pending_filename;
  std::string shm_name;
  int directory_fd{-1};
  int lock_fd{-1};
  int shm_fd{-1};
  void* mapping{MAP_FAILED};
  std::size_t mapped_size{};
  pid_t owner_pid{};
  std::uint64_t owner_epoch{};
  bool publisher{};
  bool shm_created{};
  bool header_constructed{};
  bool manifest_published{};
  bool pending_published{};
  bool closed{};

  ~Impl() { Cleanup(); }

  // Marks only our generation stopped and closes inherited fds without LOCK_UN.
  void Cleanup() noexcept {
    if (closed) return;
    closed = true;
    if (publisher && owner_pid == ::getpid()) {
      if (mapping != MAP_FAILED && header_constructed)
        static_cast<SharedHeader*>(mapping)->state.store(
            static_cast<std::uint64_t>(SegmentState::kStopped),
            std::memory_order_release);
      if ((manifest_published || pending_published) && directory_fd >= 0) {
        // Failed reads leave discovery intact. Never retire another generation.
        const auto remove_owned = [&](const std::string& filename) {
          auto manifest = ReadManifest(directory_fd, filename);
          if (manifest && manifest->generation == layout.generation &&
              manifest->shm_name == shm_name)
            ::unlinkat(directory_fd, filename.c_str(), 0);
        };
        if (manifest_published) remove_owned(manifest_filename);
        if (pending_published) remove_owned(pending_filename);
      }
      if (shm_created) ::shm_unlink(shm_name.c_str());
    }
    // Do not destroy shared atomics: surviving subscriber mappings retain
    // the stopped generation. A fork child merely drops its inherited refs.
    if (mapping != MAP_FAILED) ::munmap(mapping, mapped_size);
    mapping = MAP_FAILED;
    if (shm_fd >= 0) ::close(shm_fd);
    shm_fd = -1;
    // Closing, rather than LOCK_UN, preserves a parent's inherited flock
    // when a fork child destroys its copy of the publishing endpoint.
    if (lock_fd >= 0) ::close(lock_fd);
    lock_fd = -1;
    if (directory_fd >= 0) ::close(directory_fd);
    directory_fd = -1;
  }
};

Result<Generation> RandomGeneration() { return ReadRandomGeneration(); }

std::uint64_t MonotonicNs() noexcept {
  struct timespec value{};
  if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
  return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
         static_cast<std::uint64_t>(value.tv_nsec);
}

Result<Manifest> Discover(const Endpoint& endpoint) {
  auto validated = ValidateEndpoint(endpoint);
  if (!validated) return validated.error();
  auto directory = OpenRuntimeDirectory(*validated, false);
  if (!directory) return directory.error();
  FileDescriptor directory_fd(*directory);
  return ReadManifest(directory_fd.get(), validated->stream + ".manifest");
}

Result<void> ValidateConfig(const PublishConfig& config) {
  auto endpoint = ValidateEndpoint(config.endpoint);
  if (!endpoint) return endpoint.error();
  auto layout = MakeLayout(config.capacity, config.max_payload_size, {});
  if (!layout) return layout.error();
  return {};
}

Segment::Segment(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
Segment::~Segment() = default;

Result<std::unique_ptr<Segment>> Segment::Create(const PublishConfig& config) {
  auto endpoint = ValidateEndpoint(config.endpoint);
  if (!endpoint) return endpoint.error();
  auto layout = MakeLayout(config.capacity, config.max_payload_size, {});
  if (!layout) return layout.error();
  auto fork_guard = PrepareForkGuard();
  if (!fork_guard) return fork_guard.error();
  std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
  if (!impl) return base::Status{base::ErrorCode::kResourceExhausted, {}};
  impl->endpoint = std::move(*endpoint);
  impl->manifest_filename = impl->endpoint.stream + ".manifest";
  impl->pending_filename = impl->endpoint.stream + ".pending";
  impl->layout = *layout;
  impl->publisher = true;
  impl->owner_pid = ::getpid();
  impl->owner_epoch = process_epoch.load(std::memory_order_relaxed);
  auto directory = OpenRuntimeDirectory(impl->endpoint, true);
  if (!directory) return directory.error();
  impl->directory_fd = *directory;
  const std::string lock_filename = impl->endpoint.stream + ".lock";
  impl->lock_fd = ::openat(impl->directory_fd, lock_filename.c_str(),
                           O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (impl->lock_fd < 0) return SystemError();
  struct stat lock_info{};
  if (::fstat(impl->lock_fd, &lock_info) != 0) return SystemError();
  if (!S_ISREG(lock_info.st_mode) || lock_info.st_uid != ::geteuid() ||
      lock_info.st_nlink != 1 || (lock_info.st_mode & 0022) != 0)
    return MakeError(base::ErrorCode::kInvalidArgument);
  if (::flock(impl->lock_fd, LOCK_EX | LOCK_NB) != 0) {
    const int number = errno;
    if (number == EWOULDBLOCK || number == EAGAIN)
      return MakeError(base::ErrorCode::kInvalidState, number);
    return SystemError(number);
  }
  // Only a holder of this stable-inode lock may retire the previous instance.
  for (const auto& filename :
       {impl->pending_filename, impl->manifest_filename}) {
    auto previous = ReadManifest(impl->directory_fd, filename);
    if (previous) {
      if (::shm_unlink(previous->shm_name.c_str()) != 0 && errno != ENOENT)
        return SystemError();
      if (::unlinkat(impl->directory_fd, filename.c_str(), 0) != 0 &&
          errno != ENOENT)
        return SystemError();
    } else if (previous.error().system_error.value() != ENOENT) {
      return previous.error();
    }
  }
  for (unsigned attempt = 0; attempt != 16; ++attempt) {
    auto generation = RandomGeneration();
    if (!generation) return generation.error();
    impl->layout.generation = *generation;
    impl->shm_name = SharedName(*generation);
    // Persist the intended name before creating it so a restart can retire
    // even an instance that died before publishing its discovery manifest.
    auto pending = WriteManifest(impl->directory_fd, impl->pending_filename,
                                 {*generation, impl->shm_name});
    if (!pending) return pending.error();
    impl->pending_published = true;
    impl->shm_fd =
        ::shm_open(impl->shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (impl->shm_fd >= 0) break;
    if (errno != EEXIST) return SystemError();
    if (::unlinkat(impl->directory_fd, impl->pending_filename.c_str(), 0) !=
            0 &&
        errno != ENOENT)
      return SystemError();
    impl->pending_published = false;
  }
  if (impl->shm_fd < 0) return MakeError(base::ErrorCode::kIoError, EEXIST);
  impl->shm_created = true;
  if (::fcntl(impl->shm_fd, F_SETFD, FD_CLOEXEC) != 0) return SystemError();
  impl->mapped_size = static_cast<std::size_t>(impl->layout.segment_size);
  if (::ftruncate(impl->shm_fd, static_cast<off_t>(impl->mapped_size)) != 0)
    return SystemError();
#if defined(__linux__)
  // A successful sparse ftruncate does not reserve tmpfs pages. Reserve them
  // before touching the mapping so insufficient /dev/shm space returns ENOSPC
  // here instead of asynchronously killing the process with SIGBUS.
  int allocated;
  do {
    allocated = ::posix_fallocate(impl->shm_fd, 0,
                                  static_cast<off_t>(impl->mapped_size));
  } while (allocated == EINTR);
  if (allocated != 0) return SystemError(allocated);
#endif
  impl->mapping = ::mmap(nullptr, impl->mapped_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, impl->shm_fd, 0);
  if (impl->mapping == MAP_FAILED) return SystemError();
  if (reinterpret_cast<std::uintptr_t>(impl->mapping) % alignof(SharedHeader) !=
      0)
    return MakeError(base::ErrorCode::kUnsupported);
  auto* header = ::new (impl->mapping) SharedHeader{};
  impl->header_constructed = true;
  header->bootstrap = impl->layout;
  auto* slots =
      static_cast<std::byte*>(impl->mapping) + impl->layout.slots_offset;
  for (std::size_t i = 0; i != config.capacity; ++i) {
    auto* base = slots + i * impl->layout.slot_stride;
    for (std::size_t j = 0; j != impl->layout.word_count + 1; ++j)
      ::new (base + j * sizeof(AtomicWord)) AtomicWord{0};
  }
  header->heartbeat_ns.store(MonotonicNs(), std::memory_order_relaxed);
  header->state.store(static_cast<std::uint64_t>(SegmentState::kReady),
                      std::memory_order_release);
  auto published = WriteManifest(impl->directory_fd, impl->manifest_filename,
                                 {impl->layout.generation, impl->shm_name});
  if (!published) return published.error();
  impl->manifest_published = true;
  if (::unlinkat(impl->directory_fd, impl->pending_filename.c_str(), 0) != 0 &&
      errno != ENOENT)
    return SystemError();
  impl->pending_published = false;
  auto* segment = new (std::nothrow) Segment(std::move(impl));
  if (!segment) return base::Status{base::ErrorCode::kResourceExhausted, {}};
  return std::unique_ptr<Segment>(segment);
}

Result<std::unique_ptr<Segment>> Segment::Attach(const Endpoint& endpoint) {
  auto validated = ValidateEndpoint(endpoint);
  if (!validated) return validated.error();
  base::Status last = MakeError(base::ErrorCode::kInvalidState);
  for (unsigned attempt = 0; attempt != kDiscoveryAttempts; ++attempt) {
    std::unique_ptr<Impl> impl(new (std::nothrow) Impl);
    if (!impl) return base::Status{base::ErrorCode::kResourceExhausted, {}};
    impl->endpoint = *validated;
    impl->manifest_filename = impl->endpoint.stream + ".manifest";
    auto directory = OpenRuntimeDirectory(impl->endpoint, false);
    if (!directory) return directory.error();
    impl->directory_fd = *directory;
    auto manifest = ReadManifest(impl->directory_fd, impl->manifest_filename);
    if (!manifest) {
      if (manifest.error().system_error.value() != ENOENT)
        return manifest.error();
      last = manifest.error();
      continue;
    }
    impl->shm_name = manifest->shm_name;
    impl->shm_fd = ::shm_open(impl->shm_name.c_str(), O_RDWR, 0);
    if (impl->shm_fd < 0) {
      if (errno != ENOENT) return SystemError();
      last = SystemError();
      continue;
    }
    if (::fcntl(impl->shm_fd, F_SETFD, FD_CLOEXEC) != 0) return SystemError();
    struct stat object_info{};
    if (::fstat(impl->shm_fd, &object_info) != 0) return SystemError();
    if (!SharedObjectMode(object_info.st_mode) ||
        object_info.st_uid != ::geteuid() ||
        (object_info.st_mode & 0022) != 0 ||
        object_info.st_size < static_cast<off_t>(sizeof(SharedHeader)) ||
        static_cast<std::uintmax_t>(object_info.st_size) > kMaxSegmentBytes)
      return MakeError(base::ErrorCode::kCorrupt);
    impl->mapped_size = static_cast<std::size_t>(object_info.st_size);
    impl->mapping = ::mmap(nullptr, impl->mapped_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, impl->shm_fd, 0);
    if (impl->mapping == MAP_FAILED) return SystemError();
    struct stat mapped_info{};
    if (::fstat(impl->shm_fd, &mapped_info) != 0) return SystemError();
    if (!SameObject(object_info, mapped_info)) {
      last = MakeError(base::ErrorCode::kInvalidState);
      continue;
    }
    if (reinterpret_cast<std::uintptr_t>(impl->mapping) %
            alignof(SharedHeader) !=
        0)
      return MakeError(base::ErrorCode::kUnsupported);
    Bootstrap boot;
    std::memcpy(&boot, impl->mapping, sizeof(boot));
    auto bootstrap = ValidateBootstrap(boot, impl->mapped_size);
    if (!bootstrap) return bootstrap.error();
    // The ABI has now established the state atomic's fixed offset/alignment.
    auto* shared_header = static_cast<SharedHeader*>(impl->mapping);
    if (shared_header->state.load(std::memory_order_acquire) !=
        static_cast<std::uint64_t>(SegmentState::kReady)) {
      last = MakeError(base::ErrorCode::kInvalidState);
      continue;
    }
    // Copy immutable metadata again after acquiring Ready, then fully check
    // its layout before computing any slot addresses.
    std::memcpy(&boot, impl->mapping, sizeof(boot));
    bootstrap = ValidateBootstrap(boot, impl->mapped_size);
    if (!bootstrap) return bootstrap.error();
    auto checked_layout =
        ValidateLayout(boot, impl->mapped_size, manifest->generation);
    if (!checked_layout) return checked_layout.error();
    impl->layout = boot;
    auto current = ReadManifest(impl->directory_fd, impl->manifest_filename);
    if (!current || current->generation != manifest->generation ||
        current->shm_name != manifest->shm_name) {
      if (!current && current.error().system_error.value() != ENOENT)
        return current.error();
      last = MakeError(base::ErrorCode::kInvalidState);
      continue;
    }
    FileDescriptor named_fd(::shm_open(impl->shm_name.c_str(), O_RDWR, 0));
    if (named_fd.get() < 0) {
      if (errno != ENOENT) return SystemError();
      last = MakeError(base::ErrorCode::kInvalidState);
      continue;
    }
    struct stat named_info{};
    if (::fstat(named_fd.get(), &named_info) != 0) return SystemError();
    if (!SameObject(object_info, named_info) ||
        shared_header->state.load(std::memory_order_acquire) !=
            static_cast<std::uint64_t>(SegmentState::kReady)) {
      last = MakeError(base::ErrorCode::kInvalidState);
      continue;
    }
    auto* segment = new (std::nothrow) Segment(std::move(impl));
    if (!segment) return base::Status{base::ErrorCode::kResourceExhausted, {}};
    return std::unique_ptr<Segment>(segment);
  }
  return std::move(last);
}

SharedHeader& Segment::header() const noexcept {
  return *static_cast<SharedHeader*>(impl_->mapping);
}

AtomicWord& Segment::stamp(std::uint64_t sequence) const noexcept {
  const auto index = (sequence - 1) & (impl_->layout.capacity - 1);
  auto* address = static_cast<std::byte*>(impl_->mapping) +
                  impl_->layout.slots_offset +
                  index * impl_->layout.slot_stride;
  return *reinterpret_cast<AtomicWord*>(address);
}

AtomicWord& Segment::word(std::uint64_t sequence,
                          std::size_t index) const noexcept {
  auto* address = reinterpret_cast<std::byte*>(&stamp(sequence)) +
                  (index + 1) * sizeof(AtomicWord);
  return *reinterpret_cast<AtomicWord*>(address);
}

const Bootstrap& Segment::layout() const noexcept { return impl_->layout; }
const Endpoint& Segment::endpoint() const noexcept { return impl_->endpoint; }

bool Segment::owned_by_current_process() const noexcept {
  return impl_ && !impl_->closed &&
         (!impl_->publisher ||
          impl_->owner_epoch == process_epoch.load(std::memory_order_relaxed));
}

void Segment::Close() noexcept {
  if (impl_) impl_->Cleanup();
}

}  // namespace yquant::md::internal
