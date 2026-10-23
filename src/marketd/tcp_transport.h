#ifndef YQUANT_MARKETD_TCP_TRANSPORT_H_
#define YQUANT_MARKETD_TCP_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

#include <memory>

#include "marketd/pipeline.h"

namespace yquant::marketd {

inline constexpr size_t kMaxTcpClients = 16;

// Numeric IPv4 address and port. The listener can use port zero for an
// ephemeral port; a source always requires an explicit nonzero destination.
struct TcpEndpoint {
  char address[16] = "127.0.0.1";
  uint16_t port = 0;
};

// TCP fanout with per-client bounded queues. A slow/disconnected client is
// closed independently, so other outputs keep progressing. Default
// construction does not allocate; Init commits ownership only on success.
class TcpPublisher final : public Publisher {
 public:
  TcpPublisher(const TcpPublisher&) = delete;
  TcpPublisher& operator=(const TcpPublisher&) = delete;

  TcpPublisher() noexcept;
  ~TcpPublisher() noexcept override;
  [[nodiscard]] base::Status Init(const TcpEndpoint& endpoint,
                                  size_t max_clients = 16) noexcept;
  [[nodiscard]] base::Status Publish(
      const model::QuoteTick& quote) noexcept override;
  [[nodiscard]] base::Status Service(uint64_t now) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override;
  uint16_t port() const noexcept;
  size_t connected_clients() const noexcept;
  uint64_t disconnected_clients() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  char error_detail_[256] = {};
};

// One nonblocking TCP input. Connection loss, invalid frames or connect
// timeout are terminal, requiring a supervised restart and strategy recovery;
// the source does not silently reconnect across generations.
class TcpSource final : public Source {
 public:
  TcpSource(const TcpSource&) = delete;
  TcpSource& operator=(const TcpSource&) = delete;

  TcpSource() noexcept;
  ~TcpSource() noexcept override;
  [[nodiscard]] base::Status Init(const TcpEndpoint& endpoint) noexcept;
  [[nodiscard]] PollResult Poll(uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  char error_detail_[256] = {};
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_TCP_TRANSPORT_H_
