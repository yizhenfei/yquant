#include "yquant/trading/connection.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <system_error>
#include <utility>

#include "yquant/base/format.h"

namespace yquant::trading {
namespace {

// Both EINTR and successful partial transfers consume budget, so a busy peer
// cannot keep a single serialized connection on the event loop indefinitely.
constexpr size_t kIoBudget = 16;
constexpr uint64_t kNanosecondsPerMillisecond = 1000000;
constexpr uint64_t kNanosecondsPerSecond = 1000000000;

base::Status Failure(char (&detail)[256], base::ErrorCode code,
                     const char* operation, int error = 0) noexcept {
  if (error != 0) {
    ::yquant::base::FormatTo(detail, sizeof(detail), "{}: {}", operation,
                             std::strerror(error));
  } else {
    ::yquant::base::FormatTo(detail, sizeof(detail), "{}", operation);
  }
  return {code, error ? std::error_code(error, std::generic_category())
                      : std::error_code{}};
}

base::Status IoError() noexcept {
  return {base::ErrorCode::kIoError,
          std::error_code(errno, std::generic_category())};
}

// All address resolution is numeric and performed before ownership changes.
bool ParseAddress(const Endpoint& endpoint, sockaddr_in& address) noexcept {
  if (std::memchr(endpoint.address, '\0', sizeof(endpoint.address)) ==
      nullptr) {
    return false;
  }
  address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(endpoint.port);
  return inet_pton(AF_INET, endpoint.address, &address.sin_addr) == 1;
}

// Descriptor ownership stays with the caller on all failures.
base::Status ConfigureSocket(int fd) noexcept {
  const int flags = fcntl(fd, F_GETFL, 0);
  const int descriptor_flags = fcntl(fd, F_GETFD, 0);
  if (flags < 0 || descriptor_flags < 0 ||
      fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0 ||
      fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
    return IoError();
  }
  const int enabled = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) !=
      0) {
    return IoError();
  }
#ifdef SO_NOSIGPIPE
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) !=
      0) {
    return IoError();
  }
#endif
  return {};
}

ssize_t Send(int fd, const std::byte* bytes, size_t size) noexcept {
#ifdef MSG_NOSIGNAL
  return send(fd, bytes, size, MSG_NOSIGNAL);
#else
  return send(fd, bytes, size, 0);
#endif
}

uint64_t MonotonicNs() noexcept {
  timespec time{};
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(time.tv_sec) * kNanosecondsPerSecond +
         static_cast<uint64_t>(time.tv_nsec);
}

}  // namespace

struct Connection::Storage {
  struct Frame {
    std::array<std::byte, kMaxFrameSize> bytes;
    size_t size = 0;
  };
  int fd = -1;
  std::array<Frame, kConnectionQueueFrames> queue;
  size_t head = 0;
  size_t count = 0;
  size_t send_offset = 0;
  uint64_t next_tx_sequence = 1;
  uint64_t next_rx_sequence = 1;
  std::array<std::byte, kMaxFrameSize> receive;
  size_t receive_size = 0;
  size_t expected_size = kFrameHeaderSize;
  bool eof = false;
  base::Status fault;

  ~Storage() noexcept {
    if (fd >= 0) {
      close(fd);
    }
  }
  // A broken stream is never reused. Retain the first fault and storage until
  // Close so repeated Poll calls report the original failure consistently.
  void Break(base::Status status) noexcept {
    if (fd >= 0) {
      close(fd);
    }
    fd = -1;
    fault = status;
    count = 0;
  }
};

Connection::Connection() noexcept = default;
Connection::~Connection() noexcept = default;

base::Status Connection::Adopt(int fd) noexcept {
  if (storage_) {
    return Failure(error_detail_, base::ErrorCode::kInvalidState,
                   "connection must be closed before Adopt");
  }
  if (fd < 0) {
    return Failure(error_detail_, base::ErrorCode::kInvalidArgument,
                   "invalid connection descriptor");
  }
  auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
  if (!storage) {
    return Failure(error_detail_, base::ErrorCode::kResourceExhausted,
                   "cannot allocate connection queue");
  }
  const auto configured = ConfigureSocket(fd);
  if (!configured.ok()) {
    return Failure(error_detail_, configured.code, "configure connection",
                   configured.system_error.value());
  }
  storage->fd = fd;
  storage_ = std::move(storage);
  error_detail_[0] = '\0';
  return {};
}

base::Status Connection::Connect(const Endpoint& endpoint,
                                 uint64_t timeout_ns) noexcept {
  if (storage_) {
    return Failure(error_detail_, base::ErrorCode::kInvalidState,
                   "connection must be closed before Connect");
  }
  sockaddr_in address{};
  if (endpoint.port == 0 || !ParseAddress(endpoint, address) ||
      address.sin_addr.s_addr == htonl(INADDR_ANY) || timeout_ns == 0) {
    return Failure(error_detail_, base::ErrorCode::kInvalidArgument,
                   "connect requires numeric IPv4, a port and a timeout");
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return Failure(error_detail_, base::ErrorCode::kIoError, "socket", errno);
  }
  const auto configured = ConfigureSocket(fd);
  if (!configured.ok()) {
    close(fd);
    return Failure(error_detail_, configured.code, "configure connection",
                   configured.system_error.value());
  }
  const auto start = MonotonicNs();
  int result =
      connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  if (result != 0 && errno != EINPROGRESS) {
    const auto status =
        Failure(error_detail_, base::ErrorCode::kIoError, "connect", errno);
    close(fd);
    return status;
  }
  while (result != 0) {
    const auto now = MonotonicNs();
    if (start == 0 || now < start || now - start >= timeout_ns) {
      const auto status = Failure(error_detail_, base::ErrorCode::kIoError,
                                  "connect timeout", ETIMEDOUT);
      close(fd);
      return status;
    }
    const auto remaining = timeout_ns - (now - start);
    const auto milliseconds = remaining / kNanosecondsPerMillisecond +
                              (remaining % kNanosecondsPerMillisecond != 0);
    const auto wait = static_cast<int>(
        milliseconds > static_cast<uint64_t>(std::numeric_limits<int>::max())
            ? std::numeric_limits<int>::max()
            : milliseconds);
    pollfd event{fd, POLLOUT, 0};
    const auto polled = poll(&event, 1, wait);
    if (polled == 0 || (polled < 0 && errno == EINTR)) {
      continue;
    }
    if (polled < 0) {
      const auto status = Failure(error_detail_, base::ErrorCode::kIoError,
                                  "poll connect", errno);
      close(fd);
      return status;
    }
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0) {
      error = errno;
    }
    if (error != 0) {
      const auto status =
          Failure(error_detail_, base::ErrorCode::kIoError, "connect", error);
      close(fd);
      return status;
    }
    result = 0;
  }
  const auto status = Adopt(fd);
  if (!status.ok()) {
    close(fd);
  }
  return status;
}

base::Status Connection::Queue(const Message& message) noexcept {
  if (!open()) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& storage = *storage_;
  if (storage.count == kConnectionQueueFrames) {
    return Failure(error_detail_, base::ErrorCode::kResourceExhausted,
                   "connection output queue is full");
  }
  if (storage.next_tx_sequence == 0) {
    return Failure(error_detail_, base::ErrorCode::kResourceExhausted,
                   "connection transmit sequence exhausted");
  }
  auto queued = message;
  queued.sequence = storage.next_tx_sequence;
  auto& frame =
      storage.queue[(storage.head + storage.count) % kConnectionQueueFrames];
  const auto status = EncodeMessage(queued, frame.bytes, frame.size);
  if (!status.ok()) {
    return Failure(error_detail_, status.code,
                   "cannot encode connection message");
  }
  ++storage.count;
  ++storage.next_tx_sequence;
  return {};
}

base::Status Connection::Service() noexcept {
  if (!storage_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  auto& storage = *storage_;
  if (!storage.fault.ok()) {
    return storage.fault;
  }
  if (storage.fd < 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  for (size_t attempt = 0; attempt < kIoBudget && storage.count != 0;
       ++attempt) {
    const auto& frame = storage.queue[storage.head];
    const auto sent = Send(storage.fd, frame.bytes.data() + storage.send_offset,
                           frame.size - storage.send_offset);
    if (sent > 0) {
      storage.send_offset += static_cast<size_t>(sent);
      if (storage.send_offset == frame.size) {
        storage.head = (storage.head + 1) % kConnectionQueueFrames;
        --storage.count;
        storage.send_offset = 0;
      }
    } else if (sent < 0 && errno == EINTR) {
      continue;
    } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      const auto status = Failure(error_detail_, base::ErrorCode::kIoError,
                                  "send connection", sent < 0 ? errno : EPIPE);
      storage.Break(status);
      return status;
    }
  }
  return {};
}

ReceiveResult Connection::Poll(Message& output) noexcept {
  if (!storage_) {
    return {ReceiveState::kError, {base::ErrorCode::kInvalidState, {}}};
  }
  auto& storage = *storage_;
  if (!storage.fault.ok()) {
    return {ReceiveState::kError, storage.fault};
  }
  if (storage.eof) {
    return {ReceiveState::kClosed, {}};
  }
  const auto serviced = Service();
  if (!serviced.ok()) {
    return {ReceiveState::kError, serviced};
  }
  for (size_t attempt = 0; attempt < kIoBudget; ++attempt) {
    // Read only the current field/frame. Coalesced following frames remain in
    // the kernel receive queue and are consumed by subsequent Poll calls.
    const auto received =
        recv(storage.fd, storage.receive.data() + storage.receive_size,
             storage.expected_size - storage.receive_size, 0);
    if (received > 0) {
      storage.receive_size += static_cast<size_t>(received);
      if (storage.receive_size != storage.expected_size) {
        continue;
      }
      if (storage.expected_size == kFrameHeaderSize) {
        storage.expected_size =
            FrameSize(std::span(storage.receive).first(kFrameHeaderSize));
        if (storage.expected_size == 0) {
          const auto status = Failure(error_detail_, base::ErrorCode::kCorrupt,
                                      "invalid connection frame header");
          storage.Break(status);
          return {ReceiveState::kError, status};
        }
        if (storage.expected_size != kFrameHeaderSize) {
          continue;
        }
      }
      Message message;
      const auto decoded = DecodeMessage(
          std::span(storage.receive).first(storage.expected_size), message);
      if (!decoded.ok() || storage.next_rx_sequence == 0 ||
          message.sequence != storage.next_rx_sequence) {
        const auto status = Failure(error_detail_, base::ErrorCode::kCorrupt,
                                    "invalid connection message or sequence");
        storage.Break(status);
        return {ReceiveState::kError, status};
      }
      ++storage.next_rx_sequence;
      storage.receive_size = 0;
      storage.expected_size = kFrameHeaderSize;
      output = message;
      return {ReceiveState::kMessage, {}};
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return {};
    }
    if (received == 0 && storage.receive_size == 0) {
      close(storage.fd);
      storage.fd = -1;
      storage.eof = true;
      storage.count = 0;
      return {ReceiveState::kClosed, {}};
    }
    const auto status = Failure(
        error_detail_,
        received == 0 ? base::ErrorCode::kCorrupt : base::ErrorCode::kIoError,
        received == 0 ? "truncated connection frame" : "receive connection",
        received < 0 ? errno : 0);
    storage.Break(status);
    return {ReceiveState::kError, status};
  }
  return {};
}

bool Connection::can_queue() const noexcept {
  return open() && storage_->count < kConnectionQueueFrames &&
         storage_->next_tx_sequence != 0;
}
bool Connection::empty() const noexcept {
  return !storage_ || storage_->count == 0;
}
bool Connection::open() const noexcept { return storage_ && storage_->fd >= 0; }
void Connection::Close() noexcept { storage_.reset(); }
const char* Connection::error_detail() const noexcept { return error_detail_; }

base::Status Listener::Open(const Endpoint& endpoint) noexcept {
  if (fd_ >= 0) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  sockaddr_in address{};
  if (!ParseAddress(endpoint, address)) {
    return {base::ErrorCode::kInvalidArgument, {}};
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return IoError();
  }
  const auto configured = ConfigureSocket(fd);
  if (!configured.ok()) {
    close(fd);
    return configured;
  }
  const int enabled = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) !=
          0 ||
      bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(fd, static_cast<int>(kMaxClients)) != 0) {
    const auto status = IoError();
    close(fd);
    return status;
  }
  socklen_t size = sizeof(address);
  if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
    const auto status = IoError();
    close(fd);
    return status;
  }
  fd_ = fd;
  port_ = ntohs(address.sin_port);
  return {};
}

base::Status Listener::Accept(Connection& connection, bool& accepted) noexcept {
  accepted = false;
  if (fd_ < 0 || connection.open()) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  const int fd = accept(fd_, nullptr, nullptr);
  if (fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return {};
    }
    return IoError();
  }
  const auto status = connection.Adopt(fd);
  if (!status.ok()) {
    close(fd);
    return status;
  }
  accepted = true;
  return {};
}

void Listener::Close() noexcept {
  if (fd_ >= 0) {
    close(fd_);
  }
  fd_ = -1;
  port_ = 0;
}

}  // namespace yquant::trading
