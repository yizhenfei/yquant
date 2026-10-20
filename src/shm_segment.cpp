#include "detail/shm_segment.hpp"

#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <pthread.h>
#include <string_view>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <type_traits>
#include <unistd.h>

namespace yquant::md::detail {
namespace {

constexpr std::size_t max_segment_bytes = std::size_t{1} << 30;
constexpr std::size_t max_manifest_bytes = 256;
constexpr unsigned discovery_attempts = 4;
constexpr std::uint64_t endian_tag = std::endian::native == std::endian::little
    ? 0x4c4954544c450001ULL : 0x424947454e440001ULL;

static_assert(std::is_standard_layout_v<Bootstrap>);
static_assert(std::is_standard_layout_v<SharedHeader>);
static_assert(alignof(AtomicWord) <= cache_alignment);
static_assert(offsetof(SharedHeader, bootstrap) == 0);
static_assert(offsetof(SharedHeader, state) % alignof(AtomicWord) == 0);

Error error(ErrorCode code, std::string message, int number = 0) {
    return {code, number, std::move(message)};
}

Error system_error(std::string operation, int number = errno) {
    return error(number == ENOENT ? ErrorCode::NotFound : ErrorCode::SystemError,
                 std::move(operation) + ": " + std::strerror(number), number);
}

// This counter is process-private: only the child changes its inherited copy.
// Checking an endpoint's epoch therefore needs no getpid syscall on each event.
AtomicWord process_epoch{0};
pthread_once_t fork_guard_once = PTHREAD_ONCE_INIT;
int fork_guard_registration_error = 0;

void after_fork_child() noexcept {
    process_epoch.fetch_add(1, std::memory_order_relaxed);
}

void register_fork_guard() noexcept {
    fork_guard_registration_error = ::pthread_atfork(nullptr, nullptr, after_fork_child);
}

Result<void> prepare_fork_guard() {
    const int once_error = ::pthread_once(&fork_guard_once, register_fork_guard);
    if (once_error != 0)
        return std::unexpected(system_error("initialize fork guard", once_error));
    if (fork_guard_registration_error != 0)
        return std::unexpected(system_error("register fork guard", fork_guard_registration_error));
    return {};
}

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) noexcept : fd_(fd) {}
    ~FileDescriptor() { if (fd_ >= 0) ::close(fd_); }
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    int get() const noexcept { return fd_; }
    int release() noexcept { const int fd = fd_; fd_ = -1; return fd; }
private:
    int fd_;
};

bool add(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (b > std::numeric_limits<std::size_t>::max() - a) return false;
    out = a + b;
    return true;
}

bool multiply(std::size_t a, std::size_t b, std::size_t& out) noexcept {
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) return false;
    out = a * b;
    return true;
}

bool align_up(std::size_t value, std::size_t alignment, std::size_t& out) noexcept {
    std::size_t sum;
    if (!add(value, alignment - 1, sum)) return false;
    out = sum & ~(alignment - 1);
    return true;
}

constexpr std::uint64_t mix(std::uint64_t hash, std::uint64_t value) noexcept {
    for (unsigned i = 0; i != 8; ++i) {
        hash ^= (value >> (8 * i)) & 0xff;
        hash *= 1099511628211ULL;
    }
    return hash;
}

constexpr std::uint64_t atomic_abi() noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
#if defined(_LIBCPP_VERSION)
    hash = mix(hash, 1);
    hash = mix(hash, _LIBCPP_VERSION);
#if defined(_LIBCPP_ABI_VERSION)
    hash = mix(hash, _LIBCPP_ABI_VERSION);
#endif
#elif defined(__GLIBCXX__)
    hash = mix(hash, 2);
    hash = mix(hash, __GLIBCXX__);
#if defined(_GLIBCXX_USE_CXX11_ABI)
    hash = mix(hash, _GLIBCXX_USE_CXX11_ABI);
#endif
#else
    hash = mix(hash, 0);
#endif
    hash = mix(hash, sizeof(AtomicWord));
    hash = mix(hash, alignof(AtomicWord));
    hash = mix(hash, sizeof(Bootstrap));
    hash = mix(hash, sizeof(SharedHeader));
    hash = mix(hash, alignof(SharedHeader));
    hash = mix(hash, offsetof(SharedHeader, state));
    hash = mix(hash, offsetof(SharedHeader, published_sequence));
    return mix(hash, offsetof(SharedHeader, heartbeat_ns));
}

Result<Bootstrap> make_layout(std::size_t capacity, std::size_t payload_size,
                              Generation generation) {
    if (capacity == 0 || !std::has_single_bit(capacity))
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "capacity must be a nonzero power of two"));
    const long system_page_size = ::sysconf(_SC_PAGESIZE);
    if (system_page_size <= 0 || !std::has_single_bit(static_cast<std::size_t>(system_page_size)))
        return std::unexpected(error(ErrorCode::UnsupportedPlatform,
                                     "system page size is unavailable or is not a power of two"));
    std::size_t bytes, rounded, words, atomic_count, slot_bytes, stride, offset, total;
    if (!add(wire_header_size, payload_size, bytes) ||
        !add(bytes, sizeof(std::uint64_t) - 1, rounded))
        return std::unexpected(error(ErrorCode::InvalidArgument, "event size overflows"));
    words = rounded / sizeof(std::uint64_t);
    if (!add(words, 1, atomic_count) ||
        !multiply(atomic_count, sizeof(AtomicWord), slot_bytes) ||
        !align_up(slot_bytes, cache_alignment, stride) ||
        !align_up(sizeof(SharedHeader), cache_alignment, offset) ||
        !multiply(capacity, stride, bytes) || !add(offset, bytes, total) ||
        !align_up(total, static_cast<std::size_t>(system_page_size), total) ||
        total > max_segment_bytes ||
        total > static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "shared memory size exceeds the 1 GiB limit or overflows"));
    return Bootstrap{segment_magic, layout_version, event_schema_version, atomic_abi(),
                     endian_tag, sizeof(AtomicWord), alignof(AtomicWord),
                     sizeof(SharedHeader), total, capacity, payload_size, words,
                     stride, offset, generation};
}

Result<Endpoint> validate_endpoint(const Endpoint& endpoint) {
    if (endpoint.stream.empty() || endpoint.stream.size() > 64)
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "stream name must contain between 1 and 64 ASCII characters"));
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!alnum(static_cast<unsigned char>(endpoint.stream.front())))
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "stream name must start with an ASCII letter or digit"));
    for (unsigned char c : endpoint.stream) {
        if (!alnum(c) && c != '_' && c != '-' && c != '.')
            return std::unexpected(error(ErrorCode::InvalidArgument,
                                         "stream name contains an invalid character"));
    }
    if (endpoint.runtime_directory.empty() || !endpoint.runtime_directory.is_absolute() ||
        endpoint.runtime_directory.native().find('\0') != std::string::npos)
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "runtime_directory must be an absolute path without NUL bytes"));
    for (const auto& part : endpoint.runtime_directory) {
        if (part == "..")
            return std::unexpected(error(ErrorCode::InvalidArgument,
                                         "runtime_directory must not contain parent traversal"));
    }
    auto path = endpoint.runtime_directory.lexically_normal();
    while (path != path.root_path() && path.filename().empty()) path = path.parent_path();
    return Endpoint{std::move(path), endpoint.stream};
}

Result<int> open_runtime_directory(const Endpoint& endpoint, bool create) {
    bool created = false;
    if (create) {
        std::error_code ec;
        created = std::filesystem::create_directories(endpoint.runtime_directory, ec);
        if (ec) return std::unexpected(system_error("create runtime directory", ec.value()));
    }
    FileDescriptor fd(::open(endpoint.runtime_directory.c_str(),
                             O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (fd.get() < 0) return std::unexpected(system_error("open runtime directory"));
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0)
        return std::unexpected(system_error("stat runtime directory"));
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid())
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "runtime directory must be owned by this user and not writable by group or others"));
    if (created && ::fchmod(fd.get(), 0700) != 0)
        return std::unexpected(system_error("secure runtime directory permissions"));
    if (!created && (info.st_mode & 0022) != 0)
        return std::unexpected(error(ErrorCode::InvalidArgument,
                                     "runtime directory must not be writable by group or others"));
    return fd.release();
}

std::string hex(std::uint64_t value, unsigned digits = 16) {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string result(digits, '0');
    for (unsigned i = 0; i != digits; ++i) {
        result[digits - 1 - i] = alphabet[value & 15];
        value >>= 4;
    }
    return result;
}

std::string shared_name(Generation generation) {
    // Darwin limits shm names to 31 bytes. Retain 96 random bits in the name;
    // the full 128-bit generation is checked in the manifest and mapped header.
    return "/yq" + hex(generation.high, 8) + hex(generation.low);
}

Result<Generation> random_generation() {
    FileDescriptor fd(::open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (fd.get() < 0) return std::unexpected(system_error("open random source"));
    Generation generation;
    auto* target = reinterpret_cast<std::byte*>(&generation);
    std::size_t remaining = sizeof(generation);
    while (remaining != 0) {
        const ssize_t count = ::read(fd.get(), target, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0)
            return std::unexpected(system_error("read random generation", count == 0 ? EIO : errno));
        target += count;
        remaining -= static_cast<std::size_t>(count);
    }
    // Zero is reserved for an absent generation in client-side state.
    if (generation == Generation{}) generation.low = 1;
    return generation;
}

bool parse_hex(std::string_view value, std::uint64_t& result) noexcept {
    if (value.size() != 16) return false;
    for (char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, 16);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size();
}

Result<Manifest> read_manifest(int directory, const std::string& filename) {
    FileDescriptor fd(::openat(directory, filename.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (fd.get() < 0) return std::unexpected(system_error("open discovery manifest"));
    struct stat info{};
    if (::fstat(fd.get(), &info) != 0)
        return std::unexpected(system_error("stat discovery manifest"));
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        (info.st_mode & 0022) != 0 || info.st_size <= 0 ||
        info.st_size > static_cast<off_t>(max_manifest_bytes))
        return std::unexpected(error(ErrorCode::Incompatible, "invalid discovery manifest file"));
    std::array<char, max_manifest_bytes + 1> bytes{};
    std::size_t used = 0;
    while (used < bytes.size()) {
        const ssize_t count = ::read(fd.get(), bytes.data() + used, bytes.size() - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::unexpected(system_error("read discovery manifest"));
        if (count == 0) break;
        used += static_cast<std::size_t>(count);
    }
    if (used != static_cast<std::size_t>(info.st_size) || used > max_manifest_bytes)
        return std::unexpected(error(ErrorCode::Incompatible, "discovery manifest changed size"));
    const std::string_view text(bytes.data(), used);
    constexpr std::string_view prefix = "yquant-shm-v1\n";
    if (!text.starts_with(prefix) || text.size() <= prefix.size() + 33 || text.back() != '\n')
        return std::unexpected(error(ErrorCode::Incompatible, "invalid discovery manifest format"));
    const auto body = text.substr(prefix.size());
    Manifest manifest;
    if (body[32] != '\n' || !parse_hex(body.substr(0, 16), manifest.generation.high) ||
        !parse_hex(body.substr(16, 16), manifest.generation.low) || manifest.generation == Generation{})
        return std::unexpected(error(ErrorCode::Incompatible, "invalid manifest generation"));
    const auto name = body.substr(33, body.size() - 34);
    if (name != shared_name(manifest.generation))
        return std::unexpected(error(ErrorCode::Incompatible, "invalid shared memory object name"));
    manifest.shm_name = std::string(name);
    return manifest;
}

Result<void> write_manifest(int directory, const std::string& filename,
                            const Manifest& manifest) {
    const std::string temporary = "." + filename + "." + hex(manifest.generation.high) +
                                  hex(manifest.generation.low);
    const std::string body = "yquant-shm-v1\n" + hex(manifest.generation.high) +
                             hex(manifest.generation.low) + "\n" + manifest.shm_name + "\n";
    FileDescriptor fd(::openat(directory, temporary.c_str(),
                              O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (fd.get() < 0) return std::unexpected(system_error("create temporary discovery manifest"));
    struct TemporaryEntry {
        int directory;
        const std::string& name;
        bool active{true};
        ~TemporaryEntry() { if (active) ::unlinkat(directory, name.c_str(), 0); }
    } temporary_entry{directory, temporary};
    std::size_t written = 0;
    while (written != body.size()) {
        const ssize_t count = ::write(fd.get(), body.data() + written, body.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            const int number = count == 0 ? EIO : errno;
            return std::unexpected(system_error("write discovery manifest", number));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::renameat(directory, temporary.c_str(), directory, filename.c_str()) != 0) {
        const int number = errno;
        return std::unexpected(system_error("publish discovery manifest", number));
    }
    temporary_entry.active = false;
    return {};
}

Result<void> validate_bootstrap(const Bootstrap& boot, std::size_t mapped_size) {
    // Only fixed-width, non-atomic bytes have been inspected at this point.
    if (boot.magic != segment_magic || boot.version != layout_version ||
        boot.schema != event_schema_version || boot.abi != atomic_abi() ||
        boot.endian != endian_tag || boot.atomic_size != sizeof(AtomicWord) ||
        boot.atomic_alignment != alignof(AtomicWord) || boot.header_size != sizeof(SharedHeader) ||
        mapped_size < sizeof(SharedHeader) ||
        offsetof(SharedHeader, state) > mapped_size - sizeof(AtomicWord))
        return std::unexpected(error(ErrorCode::Incompatible,
                                     "unsupported shared memory bootstrap or atomic ABI"));
    return {};
}

Result<void> validate_layout(const Bootstrap& boot, std::size_t mapped_size,
                             Generation expected_generation) {
    if (boot.capacity > std::numeric_limits<std::size_t>::max() ||
        boot.max_payload_size > std::numeric_limits<std::size_t>::max())
        return std::unexpected(error(ErrorCode::Incompatible, "shared layout exceeds address space"));
    auto expected = make_layout(static_cast<std::size_t>(boot.capacity),
                                static_cast<std::size_t>(boot.max_payload_size), expected_generation);
    if (!expected || boot.generation != expected_generation ||
        boot.segment_size != mapped_size || boot.segment_size != expected->segment_size ||
        boot.header_size != expected->header_size || boot.word_count != expected->word_count ||
        boot.slot_stride != expected->slot_stride || boot.slots_offset != expected->slots_offset)
        return std::unexpected(error(ErrorCode::Incompatible, "invalid shared memory layout or generation"));
    return {};
}

bool same_object(const struct stat& a, const struct stat& b) noexcept {
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size;
}

bool shared_object_mode(mode_t mode) noexcept {
    if (S_ISREG(mode)) return true;
#if defined(__APPLE__)
    // Darwin's POSIX shm implementation reports permission bits without an
    // S_IFREG type. The descriptor was obtained through shm_open, not open.
    return (mode & S_IFMT) == 0;
#else
    return false;
#endif
}

} // namespace

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

    ~Impl() { cleanup(); }

    void cleanup() noexcept {
        if (closed) return;
        closed = true;
        if (publisher && owner_pid == ::getpid()) {
            if (mapping != MAP_FAILED && header_constructed)
                static_cast<SharedHeader*>(mapping)->state.store(
                    static_cast<std::uint64_t>(SegmentState::Stopped), std::memory_order_release);
            if ((manifest_published || pending_published) && directory_fd >= 0) {
                // A cleanup failure cannot escape a destructor. In particular,
                // never remove a discovery entry that names another generation.
                try {
                    const auto remove_owned = [&](const std::string& filename) {
                        auto manifest = read_manifest(directory_fd, filename);
                        if (manifest && manifest->generation == layout.generation &&
                            manifest->shm_name == shm_name)
                            ::unlinkat(directory_fd, filename.c_str(), 0);
                    };
                    if (manifest_published) remove_owned(manifest_filename);
                    if (pending_published) remove_owned(pending_filename);
                } catch (...) {}
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

std::uint64_t monotonic_ns() noexcept {
    struct timespec value{};
    if (::clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(value.tv_nsec);
}

Result<Manifest> discover(const Endpoint& endpoint) {
    auto validated = validate_endpoint(endpoint);
    if (!validated) return std::unexpected(validated.error());
    auto directory = open_runtime_directory(*validated, false);
    if (!directory) return std::unexpected(directory.error());
    FileDescriptor directory_fd(*directory);
    return read_manifest(directory_fd.get(), validated->stream + ".manifest");
}

Result<void> validate_config(const PublishConfig& config) {
    auto endpoint = validate_endpoint(config.endpoint);
    if (!endpoint) return std::unexpected(endpoint.error());
    auto layout = make_layout(config.capacity, config.max_payload_size, {});
    if (!layout) return std::unexpected(layout.error());
    return {};
}

Segment::Segment(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Segment::~Segment() = default;

Result<std::unique_ptr<Segment>> Segment::create(const PublishConfig& config) {
    auto endpoint = validate_endpoint(config.endpoint);
    if (!endpoint) return std::unexpected(endpoint.error());
    auto layout = make_layout(config.capacity, config.max_payload_size, {});
    if (!layout) return std::unexpected(layout.error());
    auto fork_guard = prepare_fork_guard();
    if (!fork_guard) return std::unexpected(fork_guard.error());
    auto impl = std::make_unique<Impl>();
    impl->endpoint = std::move(*endpoint);
    impl->manifest_filename = impl->endpoint.stream + ".manifest";
    impl->pending_filename = impl->endpoint.stream + ".pending";
    impl->layout = *layout;
    impl->publisher = true;
    impl->owner_pid = ::getpid();
    impl->owner_epoch = process_epoch.load(std::memory_order_relaxed);
    auto directory = open_runtime_directory(impl->endpoint, true);
    if (!directory) return std::unexpected(directory.error());
    impl->directory_fd = *directory;
    const std::string lock_filename = impl->endpoint.stream + ".lock";
    impl->lock_fd = ::openat(impl->directory_fd, lock_filename.c_str(),
                             O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (impl->lock_fd < 0) return std::unexpected(system_error("open publisher lock"));
    struct stat lock_info{};
    if (::fstat(impl->lock_fd, &lock_info) != 0)
        return std::unexpected(system_error("stat publisher lock"));
    if (!S_ISREG(lock_info.st_mode) || lock_info.st_uid != ::geteuid() ||
        lock_info.st_nlink != 1 || (lock_info.st_mode & 0022) != 0)
        return std::unexpected(error(ErrorCode::InvalidArgument, "unsafe publisher lock file"));
    if (::flock(impl->lock_fd, LOCK_EX | LOCK_NB) != 0) {
        const int number = errno;
        if (number == EWOULDBLOCK || number == EAGAIN)
            return std::unexpected(error(ErrorCode::AlreadyPublished,
                                         "the logical stream already has a publisher", number));
        return std::unexpected(system_error("acquire publisher lock", number));
    }
    // Only a holder of this stable-inode lock may retire the previous instance.
    for (const auto& filename : {impl->pending_filename, impl->manifest_filename}) {
        auto previous = read_manifest(impl->directory_fd, filename);
        if (previous) {
            if (::shm_unlink(previous->shm_name.c_str()) != 0 && errno != ENOENT)
                return std::unexpected(system_error("unlink previous shared memory name"));
            if (::unlinkat(impl->directory_fd, filename.c_str(), 0) != 0 && errno != ENOENT)
                return std::unexpected(system_error("retire previous discovery record"));
        } else if (previous.error().code != ErrorCode::NotFound) {
            return std::unexpected(previous.error());
        }
    }
    for (unsigned attempt = 0; attempt != 16; ++attempt) {
        auto generation = random_generation();
        if (!generation) return std::unexpected(generation.error());
        impl->layout.generation = *generation;
        impl->shm_name = shared_name(*generation);
        // Persist the intended name before creating it so a restart can retire
        // even an instance that died before publishing its discovery manifest.
        auto pending = write_manifest(impl->directory_fd, impl->pending_filename,
                                      {*generation, impl->shm_name});
        if (!pending) return std::unexpected(pending.error());
        impl->pending_published = true;
        impl->shm_fd = ::shm_open(impl->shm_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (impl->shm_fd >= 0) break;
        if (errno != EEXIST) return std::unexpected(system_error("create shared memory"));
        if (::unlinkat(impl->directory_fd, impl->pending_filename.c_str(), 0) != 0 && errno != ENOENT)
            return std::unexpected(system_error("retire collided creation record"));
        impl->pending_published = false;
    }
    if (impl->shm_fd < 0)
        return std::unexpected(error(ErrorCode::SystemError, "shared memory name collisions", EEXIST));
    impl->shm_created = true;
    if (::fcntl(impl->shm_fd, F_SETFD, FD_CLOEXEC) != 0)
        return std::unexpected(system_error("set shared memory close-on-exec"));
    impl->mapped_size = static_cast<std::size_t>(impl->layout.segment_size);
    if (::ftruncate(impl->shm_fd, static_cast<off_t>(impl->mapped_size)) != 0)
        return std::unexpected(system_error("size shared memory"));
#if defined(__linux__)
    // A successful sparse ftruncate does not reserve tmpfs pages. Reserve them
    // before touching the mapping so insufficient /dev/shm space returns ENOSPC
    // here instead of asynchronously killing the process with SIGBUS.
    int allocated;
    do {
        allocated = ::posix_fallocate(impl->shm_fd, 0, static_cast<off_t>(impl->mapped_size));
    } while (allocated == EINTR);
    if (allocated != 0)
        return std::unexpected(system_error("reserve shared memory pages", allocated));
#endif
    impl->mapping = ::mmap(nullptr, impl->mapped_size, PROT_READ | PROT_WRITE,
                           MAP_SHARED, impl->shm_fd, 0);
    if (impl->mapping == MAP_FAILED) return std::unexpected(system_error("map publishing segment"));
    if (reinterpret_cast<std::uintptr_t>(impl->mapping) % alignof(SharedHeader) != 0)
        return std::unexpected(error(ErrorCode::UnsupportedPlatform, "mmap does not satisfy shared alignment"));
    auto* header = ::new (impl->mapping) SharedHeader{};
    impl->header_constructed = true;
    header->bootstrap = impl->layout;
    auto* slots = static_cast<std::byte*>(impl->mapping) + impl->layout.slots_offset;
    for (std::size_t i = 0; i != config.capacity; ++i) {
        auto* base = slots + i * impl->layout.slot_stride;
        for (std::size_t j = 0; j != impl->layout.word_count + 1; ++j)
            ::new (base + j * sizeof(AtomicWord)) AtomicWord{0};
    }
    header->heartbeat_ns.store(monotonic_ns(), std::memory_order_relaxed);
    header->state.store(static_cast<std::uint64_t>(SegmentState::Ready), std::memory_order_release);
    auto published = write_manifest(impl->directory_fd, impl->manifest_filename,
                                    {impl->layout.generation, impl->shm_name});
    if (!published) return std::unexpected(published.error());
    impl->manifest_published = true;
    if (::unlinkat(impl->directory_fd, impl->pending_filename.c_str(), 0) != 0 && errno != ENOENT)
        return std::unexpected(system_error("retire shared memory creation record"));
    impl->pending_published = false;
    return std::unique_ptr<Segment>(new Segment(std::move(impl)));
}

Result<std::unique_ptr<Segment>> Segment::attach(const Endpoint& endpoint) {
    auto validated = validate_endpoint(endpoint);
    if (!validated) return std::unexpected(validated.error());
    Error last = error(ErrorCode::NotReady, "discovery changed while attaching");
    for (unsigned attempt = 0; attempt != discovery_attempts; ++attempt) {
        auto impl = std::make_unique<Impl>();
        impl->endpoint = *validated;
        impl->manifest_filename = impl->endpoint.stream + ".manifest";
        auto directory = open_runtime_directory(impl->endpoint, false);
        if (!directory) return std::unexpected(directory.error());
        impl->directory_fd = *directory;
        auto manifest = read_manifest(impl->directory_fd, impl->manifest_filename);
        if (!manifest) {
            if (manifest.error().code != ErrorCode::NotFound) return std::unexpected(manifest.error());
            last = manifest.error();
            continue;
        }
        impl->shm_name = manifest->shm_name;
        impl->shm_fd = ::shm_open(impl->shm_name.c_str(), O_RDWR, 0);
        if (impl->shm_fd < 0) {
            if (errno != ENOENT) return std::unexpected(system_error("open subscribing segment"));
            last = system_error("open subscribing segment");
            continue;
        }
        if (::fcntl(impl->shm_fd, F_SETFD, FD_CLOEXEC) != 0)
            return std::unexpected(system_error("set subscribing segment close-on-exec"));
        struct stat object_info{};
        if (::fstat(impl->shm_fd, &object_info) != 0)
            return std::unexpected(system_error("stat subscribing segment"));
        if (!shared_object_mode(object_info.st_mode) || object_info.st_uid != ::geteuid() ||
            (object_info.st_mode & 0022) != 0 ||
            object_info.st_size < static_cast<off_t>(sizeof(SharedHeader)) ||
            static_cast<std::uintmax_t>(object_info.st_size) > max_segment_bytes)
            return std::unexpected(error(ErrorCode::Incompatible, "invalid shared memory object size or permissions"));
        impl->mapped_size = static_cast<std::size_t>(object_info.st_size);
        impl->mapping = ::mmap(nullptr, impl->mapped_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, impl->shm_fd, 0);
        if (impl->mapping == MAP_FAILED) return std::unexpected(system_error("map subscribing segment"));
        struct stat mapped_info{};
        if (::fstat(impl->shm_fd, &mapped_info) != 0)
            return std::unexpected(system_error("restat subscribing segment"));
        if (!same_object(object_info, mapped_info)) {
            last = error(ErrorCode::NotReady, "shared memory object changed size");
            continue;
        }
        if (reinterpret_cast<std::uintptr_t>(impl->mapping) % alignof(SharedHeader) != 0)
            return std::unexpected(error(ErrorCode::UnsupportedPlatform, "mmap does not satisfy shared alignment"));
        Bootstrap boot;
        std::memcpy(&boot, impl->mapping, sizeof(boot));
        auto bootstrap = validate_bootstrap(boot, impl->mapped_size);
        if (!bootstrap) return std::unexpected(bootstrap.error());
        // The ABI has now established the state atomic's fixed offset/alignment.
        auto* shared_header = static_cast<SharedHeader*>(impl->mapping);
        if (shared_header->state.load(std::memory_order_acquire) !=
            static_cast<std::uint64_t>(SegmentState::Ready)) {
            last = error(ErrorCode::NotReady, "publisher is not ready or has stopped");
            continue;
        }
        // Copy immutable metadata again after acquiring Ready, then fully check
        // its layout before computing any slot addresses.
        std::memcpy(&boot, impl->mapping, sizeof(boot));
        bootstrap = validate_bootstrap(boot, impl->mapped_size);
        if (!bootstrap) return std::unexpected(bootstrap.error());
        auto checked_layout = validate_layout(boot, impl->mapped_size, manifest->generation);
        if (!checked_layout) return std::unexpected(checked_layout.error());
        impl->layout = boot;
        auto current = read_manifest(impl->directory_fd, impl->manifest_filename);
        if (!current || current->generation != manifest->generation ||
            current->shm_name != manifest->shm_name) {
            if (!current && current.error().code != ErrorCode::NotFound)
                return std::unexpected(current.error());
            last = error(ErrorCode::NotReady, "discovery changed while attaching");
            continue;
        }
        FileDescriptor named_fd(::shm_open(impl->shm_name.c_str(), O_RDWR, 0));
        if (named_fd.get() < 0) {
            if (errno != ENOENT) return std::unexpected(system_error("reopen discovered segment"));
            last = error(ErrorCode::NotReady, "discovered shared memory name was retired");
            continue;
        }
        struct stat named_info{};
        if (::fstat(named_fd.get(), &named_info) != 0)
            return std::unexpected(system_error("stat discovered segment identity"));
        if (!same_object(object_info, named_info) ||
            shared_header->state.load(std::memory_order_acquire) !=
                static_cast<std::uint64_t>(SegmentState::Ready)) {
            last = error(ErrorCode::NotReady, "discovered segment changed or stopped");
            continue;
        }
        return std::unique_ptr<Segment>(new Segment(std::move(impl)));
    }
    return std::unexpected(std::move(last));
}

SharedHeader& Segment::header() const noexcept {
    return *static_cast<SharedHeader*>(impl_->mapping);
}

AtomicWord& Segment::stamp(std::uint64_t sequence) const noexcept {
    const auto index = (sequence - 1) & (impl_->layout.capacity - 1);
    auto* address = static_cast<std::byte*>(impl_->mapping) + impl_->layout.slots_offset +
                    index * impl_->layout.slot_stride;
    return *reinterpret_cast<AtomicWord*>(address);
}

AtomicWord& Segment::word(std::uint64_t sequence, std::size_t index) const noexcept {
    auto* address = reinterpret_cast<std::byte*>(&stamp(sequence)) + (index + 1) * sizeof(AtomicWord);
    return *reinterpret_cast<AtomicWord*>(address);
}

const Bootstrap& Segment::layout() const noexcept { return impl_->layout; }
const Endpoint& Segment::endpoint() const noexcept { return impl_->endpoint; }

bool Segment::owned_by_current_process() const noexcept {
    return impl_ && !impl_->closed &&
           (!impl_->publisher || impl_->owner_epoch == process_epoch.load(std::memory_order_relaxed));
}

void Segment::close() noexcept { if (impl_) impl_->cleanup(); }

} // namespace yquant::md::detail
