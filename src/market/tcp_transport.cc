#include "yquant/market/tcp_transport.h"

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
#include <memory>
#include <new>
#include <system_error>
#include <utility>

#include "yquant/base/format.h"
#include "yquant/market/quote_codec.h"

namespace yquant::market {
namespace {

constexpr size_t kMaxClients = kMaxTcpClients;
constexpr size_t kClientQueueCapacity = 64;
constexpr size_t kIoBudget = 16;
constexpr uint64_t kConnectTimeoutNs = 3'000'000'000;

// Diagnostics are written only on control/error paths and own their storage.
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

uint64_t MonotonicNs() noexcept {
  timespec time{};
  if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(time.tv_sec) * 1'000'000'000 + time.tv_nsec;
}

// Borrows an already created descriptor; failure does not close it.
base::Status ConfigureSocket(int descriptor) noexcept {
  const int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0 ||
      fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) {
    return {base::ErrorCode::kIoError,
            std::error_code(errno, std::generic_category())};
  }
  const int enabled = 1;
  if (setsockopt(descriptor, IPPROTO_TCP, TCP_NODELAY, &enabled,
                 sizeof(enabled)) != 0) {
    return {base::ErrorCode::kIoError,
            std::error_code(errno, std::generic_category())};
  }
#ifdef SO_NOSIGPIPE
  if (setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                 sizeof(enabled)) != 0) {
    return {base::ErrorCode::kIoError,
            std::error_code(errno, std::generic_category())};
  }
#endif
  return {};
}

// Only numeric IPv4 addresses are accepted; no DNS runs on the event thread.
bool ParseAddress(const TcpEndpoint& endpoint, sockaddr_in& out) noexcept {
  if (std::memchr(endpoint.address, '\0', sizeof(endpoint.address)) ==
      nullptr) {
    return false;
  }
  out = {};
  out.sin_family = AF_INET;
  out.sin_port = htons(endpoint.port);
  return inet_pton(AF_INET, endpoint.address, &out.sin_addr) == 1;
}

ssize_t Send(int descriptor, const std::byte* bytes, size_t size) noexcept {
#ifdef MSG_NOSIGNAL
  return send(descriptor, bytes, size, MSG_NOSIGNAL);
#else
  return send(descriptor, bytes, size, 0);
#endif
}

}  // namespace

struct TcpPublisher::Impl {
  struct Client {
    int descriptor = -1;
    std::array<market::QuoteFrame, kClientQueueCapacity> frames{};
    size_t head = 0;
    size_t count = 0;
    size_t offset = 0;
  };

  int listener = -1;
  uint16_t port = 0;
  size_t max_clients = 0;
  uint64_t disconnected = 0;
  std::array<Client, kMaxClients> clients;

  ~Impl() noexcept {
    for (auto& client : clients) {
      CloseClient(client, false);
    }
    if (listener >= 0) {
      close(listener);
    }
  }

  // Dropping this connection isolates its backpressure from all other outputs.
  void CloseClient(Client& client, bool count) noexcept {
    if (client.descriptor >= 0) {
      close(client.descriptor);
      if (count) {
        ++disconnected;
      }
    }
    client.descriptor = -1;
    client.head = client.count = client.offset = 0;
  }

  // Sends complete or partial frames within a fixed syscall budget. No other
  // client shares this queue, and a short write never drops a frame prefix.
  void Flush(Client& client) noexcept {
    for (size_t attempt = 0;
         attempt < kIoBudget && client.descriptor >= 0 && client.count != 0;
         ++attempt) {
      const auto& frame = client.frames[client.head];
      const auto sent = Send(client.descriptor, frame.data() + client.offset,
                             frame.size() - client.offset);
      if (sent > 0) {
        client.offset += static_cast<size_t>(sent);
        if (client.offset == frame.size()) {
          client.head = (client.head + 1) % kClientQueueCapacity;
          --client.count;
          client.offset = 0;
        }
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
      } else {
        CloseClient(client, true);
      }
    }
  }
};

TcpPublisher::TcpPublisher() noexcept = default;
TcpPublisher::~TcpPublisher() noexcept = default;

base::Status TcpPublisher::Init(const TcpEndpoint& endpoint,
                                size_t max_clients) noexcept {
  if (impl_) {
    return Failure(error_detail_, base::ErrorCode::kInvalidState,
                   "TCP publisher is already open");
  }
  sockaddr_in address{};
  if (max_clients == 0 || max_clients > kMaxClients ||
      !ParseAddress(endpoint, address)) {
    return Failure(error_detail_, base::ErrorCode::kInvalidArgument,
                   "TCP publisher requires IPv4 and 1..16 clients");
  }
  auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl);
  if (!impl) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  impl->max_clients = max_clients;
  impl->listener = socket(AF_INET, SOCK_STREAM, 0);
  if (impl->listener < 0) {
    return Failure(error_detail_, base::ErrorCode::kIoError, "socket", errno);
  }
  const auto configured = ConfigureSocket(impl->listener);
  if (!configured.ok()) {
    return Failure(error_detail_, configured.code, "configure listener",
                   configured.system_error.value());
  }
  const int enabled = 1;
  if (setsockopt(impl->listener, SOL_SOCKET, SO_REUSEADDR, &enabled,
                 sizeof(enabled)) != 0 ||
      bind(impl->listener, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0 ||
      listen(impl->listener, static_cast<int>(max_clients)) != 0) {
    return Failure(error_detail_, base::ErrorCode::kIoError, "listen", errno);
  }
  socklen_t size = sizeof(address);
  if (getsockname(impl->listener, reinterpret_cast<sockaddr*>(&address),
                  &size) != 0) {
    return Failure(error_detail_, base::ErrorCode::kIoError, "getsockname",
                   errno);
  }
  impl->port = ntohs(address.sin_port);
  impl_ = std::move(impl);
  error_detail_[0] = '\0';
  return {};
}

base::Status TcpPublisher::Publish(const model::QuoteTick& quote) noexcept {
  if (!impl_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  market::QuoteFrame frame;
  const auto encoded = market::EncodeQuote(quote, frame);
  if (!encoded.ok()) {
    return Failure(error_detail_, encoded.code, "invalid quote for TCP output");
  }
  for (size_t index = 0; index < impl_->max_clients; ++index) {
    auto& client = impl_->clients[index];
    if (client.descriptor < 0) {
      continue;
    }
    impl_->Flush(client);
    if (client.descriptor < 0) {
      continue;
    }
    if (client.count == kClientQueueCapacity) {
      impl_->CloseClient(client, true);
      continue;
    }
    const auto tail = (client.head + client.count) % kClientQueueCapacity;
    client.frames[tail] = frame;
    ++client.count;
    impl_->Flush(client);
  }
  return {};
}

base::Status TcpPublisher::Service(uint64_t) noexcept {
  if (!impl_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  // Retire idle disconnected peers before accepting replacements. A single
  // zero-timeout poll avoids one recv syscall per healthy idle connection.
  std::array<pollfd, kMaxClients> descriptors{};
  std::array<Impl::Client*, kMaxClients> owners{};
  nfds_t descriptor_count = 0;
  for (size_t index = 0; index < impl_->max_clients; ++index) {
    auto& client = impl_->clients[index];
    if (client.descriptor >= 0) {
      descriptors[descriptor_count] = {client.descriptor, POLLIN, 0};
      owners[descriptor_count++] = &client;
    }
  }
  if (descriptor_count != 0) {
    const int result = poll(descriptors.data(), descriptor_count, 0);
    if (result < 0 && errno != EINTR) {
      return Failure(error_detail_, base::ErrorCode::kIoError,
                     "poll TCP clients", errno);
    }
    if (result > 0) {
      for (nfds_t index = 0; index < descriptor_count; ++index) {
        const auto events = descriptors[index].revents;
        if ((events & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
          impl_->CloseClient(*owners[index], true);
        } else if ((events & POLLIN) != 0) {
          std::byte byte;
          const auto count = recv(descriptors[index].fd, &byte, 1, MSG_PEEK);
          // This is a one-way protocol: peer data is invalid, EOF is closure.
          if (count >= 0 ||
              (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
            impl_->CloseClient(*owners[index], true);
          }
        }
      }
    }
  }
  for (size_t attempt = 0; attempt < kIoBudget; ++attempt) {
    const int descriptor = accept(impl_->listener, nullptr, nullptr);
    if (descriptor < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      return Failure(error_detail_, base::ErrorCode::kIoError, "accept", errno);
    }
    const auto configured = ConfigureSocket(descriptor);
    Impl::Client* free = nullptr;
    for (size_t index = 0; index < impl_->max_clients; ++index) {
      if (impl_->clients[index].descriptor < 0) {
        free = &impl_->clients[index];
        break;
      }
    }
    if (!configured.ok() || free == nullptr) {
      close(descriptor);
      ++impl_->disconnected;
    } else {
      free->descriptor = descriptor;
    }
  }
  for (size_t index = 0; index < impl_->max_clients; ++index) {
    impl_->Flush(impl_->clients[index]);
  }
  return {};
}

void TcpPublisher::Close() noexcept { impl_.reset(); }
const char* TcpPublisher::error_detail() const noexcept {
  return error_detail_;
}
uint16_t TcpPublisher::port() const noexcept { return impl_ ? impl_->port : 0; }
size_t TcpPublisher::connected_clients() const noexcept {
  if (!impl_) {
    return 0;
  }
  size_t count = 0;
  for (size_t index = 0; index < impl_->max_clients; ++index) {
    if (impl_->clients[index].descriptor >= 0) {
      ++count;
    }
  }
  return count;
}
uint64_t TcpPublisher::disconnected_clients() const noexcept {
  return impl_ ? impl_->disconnected : 0;
}

struct TcpSource::Impl {
  int descriptor = -1;
  bool connecting = false;
  bool failed = false;
  uint64_t connect_started = 0;
  market::QuoteFrame frame{};
  size_t received = 0;
  base::Status failure;

  ~Impl() noexcept {
    if (descriptor >= 0) {
      close(descriptor);
    }
  }
};

TcpSource::TcpSource() noexcept = default;
TcpSource::~TcpSource() noexcept = default;

base::Status TcpSource::Init(const TcpEndpoint& endpoint) noexcept {
  if (impl_) {
    return {base::ErrorCode::kInvalidState, {}};
  }
  sockaddr_in address{};
  if (endpoint.port == 0 || !ParseAddress(endpoint, address) ||
      address.sin_addr.s_addr == htonl(INADDR_ANY)) {
    return Failure(error_detail_, base::ErrorCode::kInvalidArgument,
                   "TCP input requires a destination IPv4 and nonzero port");
  }
  auto impl = std::unique_ptr<Impl>(new (std::nothrow) Impl);
  if (!impl) {
    return {base::ErrorCode::kResourceExhausted, {}};
  }
  impl->descriptor = socket(AF_INET, SOCK_STREAM, 0);
  if (impl->descriptor < 0) {
    return Failure(error_detail_, base::ErrorCode::kIoError, "socket", errno);
  }
  const auto configured = ConfigureSocket(impl->descriptor);
  if (!configured.ok()) {
    return Failure(error_detail_, configured.code, "configure source socket",
                   configured.system_error.value());
  }
  impl->connect_started = MonotonicNs();
  if (connect(impl->descriptor, reinterpret_cast<sockaddr*>(&address),
              sizeof(address)) != 0) {
    if (errno != EINPROGRESS && errno != EINTR) {
      return Failure(error_detail_, base::ErrorCode::kIoError, "connect",
                     errno);
    }
    impl->connecting = true;
  }
  impl_ = std::move(impl);
  error_detail_[0] = '\0';
  return {};
}

PollResult TcpSource::Poll(uint64_t now, model::QuoteTick& out) noexcept {
  if (!impl_) {
    return {PollState::kError, {base::ErrorCode::kInvalidState, {}}};
  }
  auto& impl = *impl_;
  if (impl.failed) {
    return {PollState::kError, impl.failure};
  }
  const auto fail = [&](base::ErrorCode code, const char* detail,
                        int number = 0) {
    impl.failed = true;
    impl.failure = Failure(error_detail_, code, detail, number);
    return PollResult{PollState::kError, impl.failure};
  };
  if (impl.connecting) {
    pollfd descriptor{impl.descriptor, POLLOUT, 0};
    const auto ready = poll(&descriptor, 1, 0);
    if (ready < 0) {
      if (errno == EINTR) {
        return {};
      }
      return fail(base::ErrorCode::kIoError, "poll connecting socket", errno);
    }
    if (ready == 0) {
      const auto current = MonotonicNs();
      if (current >= impl.connect_started &&
          current - impl.connect_started >= kConnectTimeoutNs) {
        return fail(base::ErrorCode::kIoError, "TCP connection timed out",
                    ETIMEDOUT);
      }
      return {};
    }
    int socket_error = 0;
    socklen_t size = sizeof(socket_error);
    if (getsockopt(impl.descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                   &size) != 0) {
      return fail(base::ErrorCode::kIoError, "read TCP connection status",
                  errno);
    }
    if (socket_error != 0) {
      return fail(base::ErrorCode::kIoError, "TCP connection failed",
                  socket_error);
    }
    impl.connecting = false;
  }
  for (size_t attempt = 0; attempt < kIoBudget; ++attempt) {
    const auto count = recv(impl.descriptor, impl.frame.data() + impl.received,
                            impl.frame.size() - impl.received, 0);
    if (count > 0) {
      impl.received += static_cast<size_t>(count);
      if (impl.received == impl.frame.size()) {
        model::QuoteTick quote;
        const auto decoded = market::DecodeQuote(impl.frame, quote);
        if (!decoded.ok()) {
          return fail(decoded.code, "invalid TCP quote frame");
        }
        impl.received = 0;
        quote.receive_time_ns = now;
        out = quote;
        return {PollState::kEvent, {}};
      }
    } else if (count == 0) {
      return fail(impl.received == 0 ? base::ErrorCode::kIoError
                                     : base::ErrorCode::kCorrupt,
                  impl.received == 0 ? "upstream TCP publisher closed"
                                     : "upstream closed within a quote frame");
    } else if (errno == EINTR) {
      continue;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {};
    } else {
      return fail(base::ErrorCode::kIoError, "receive TCP quote", errno);
    }
  }
  return {};
}

void TcpSource::Close() noexcept { impl_.reset(); }
const char* TcpSource::error_detail() const noexcept { return error_detail_; }

}  // namespace yquant::market
