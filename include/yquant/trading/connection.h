#ifndef YQUANT_TRADING_CONNECTION_H_
#define YQUANT_TRADING_CONNECTION_H_

#include <stdint.h>

#include <cstddef>
#include <memory>

#include "yquant/trading/protocol.h"

namespace yquant::trading {

enum class ReceiveState { kMessage, kIdle, kClosed, kError };
struct ReceiveResult {
  ReceiveState state = ReceiveState::kIdle;
  base::Status status;
};

// One serialized stream endpoint. Storage is allocated only by Adopt/Connect.
// Queue accepts a full frame or leaves it unqueued; no overwrite or retries of
// application commands are performed. Each direction has its own sequence.
// All operations require one owning thread; there is no internal locking.
class Connection {
 public:
  Connection() noexcept;
  ~Connection() noexcept;
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  // On success owns fd; failure leaves descriptor ownership with the caller.
  [[nodiscard]] base::Status Adopt(int fd) noexcept;
  // Cold bounded connect. Numeric IPv4 only; failure leaves the object closed.
  [[nodiscard]] base::Status Connect(const Endpoint& endpoint,
                                     uint64_t timeout_ns) noexcept;
  [[nodiscard]] base::Status Queue(const Message& message) noexcept;
  // Transfers a bounded number of queued byte ranges to the socket. A write
  // failure closes the stream and is reported again by subsequent Poll calls.
  [[nodiscard]] base::Status Service() noexcept;
  // Services pending writes and consumes at most one incoming message. All
  // non-message outcomes preserve output. Closed means peer EOF after backlog.
  [[nodiscard]] ReceiveResult Poll(Message& output) noexcept;
  bool can_queue() const noexcept;
  // Empty means the socket has accepted all queued bytes, not broker receipt.
  bool empty() const noexcept;
  bool open() const noexcept;
  // Releases the descriptor and queue; the last fault diagnostic is retained.
  void Close() noexcept;
  const char* error_detail() const noexcept;

 private:
  struct Storage;
  std::unique_ptr<Storage> storage_;
  char error_detail_[256] = {};
};

class Listener {
 public:
  Listener() noexcept = default;
  ~Listener() noexcept { Close(); }
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  // Port zero selects an ephemeral port for typed tests.
  [[nodiscard]] base::Status Open(const Endpoint& endpoint) noexcept;
  // accepted=false on an empty accept queue. Success transfers a new endpoint
  // into a closed Connection; failure closes any newly accepted descriptor.
  [[nodiscard]] base::Status Accept(Connection& connection,
                                    bool& accepted) noexcept;
  uint16_t port() const noexcept { return port_; }
  void Close() noexcept;

 private:
  int fd_ = -1;
  uint16_t port_ = 0;
};

}  // namespace yquant::trading
#endif  // YQUANT_TRADING_CONNECTION_H_
