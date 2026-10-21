#ifndef YQUANT_MARKETD_ADAPTERS_H_
#define YQUANT_MARKETD_ADAPTERS_H_

#include <cstdint>

#include "marketd/config.h"
#include "marketd/pipeline.h"
#include "yquant/md/mock_quote_source.h"
#include "yquant/md/shm_broadcast.h"

namespace yquant::marketd {

// Wraps the standalone generator and emits every configured instrument.
// Poll and Close are serialized by the owning daemon thread.
class MockSource final : public Source {
 public:
  [[nodiscard]] base::Status Init(const md::MockQuoteConfig& config,
                                  std::uint64_t now) noexcept;
  [[nodiscard]] PollResult Poll(std::uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override;

 private:
  md::MockQuoteSource source_;
};

// Attaches once at the live cursor. A gap, generation replacement, stopped or
// stale producer, or corrupt quote is terminal: recovery belongs to the host.
// Refresh is a control-path operation performed at most once per 250 ms.
class ShmSource final : public Source {
 public:
  [[nodiscard]] base::Status Init(const md::Endpoint& endpoint) noexcept;
  [[nodiscard]] PollResult Poll(std::uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Records the first terminal error and leaves future polls in that state.
  PollResult Fail(base::Status status, const char* detail) noexcept;

  md::ShmReader reader_;
  md::EventBuffer buffer_;
  md::RefreshResult refreshed_;
  std::uint64_t next_refresh_ = 0;
  base::Status failure_;
  bool active_ = false;
  char error_detail_[256] = {};
};

// Encodes one quote into preallocated storage and publishes its envelope.
// Heartbeats run through Service, never once per quote. Single-thread-owned.
class ShmPublisherAdapter final : public Publisher {
 public:
  [[nodiscard]] base::Status Init(const md::PublishConfig& config) noexcept;
  [[nodiscard]] base::Status Publish(
      const model::QuoteTick& quote) noexcept override;
  [[nodiscard]] base::Status Service(std::uint64_t now) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  md::ShmPublisher publisher_;
  std::uint64_t next_tick_ = 0;
  bool active_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_ADAPTERS_H_
