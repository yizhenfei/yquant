#ifndef YQUANT_MARKET_SOURCES_H_
#define YQUANT_MARKET_SOURCES_H_

#include <stdint.h>

#include "yquant/market/mock_quote_config.h"
#include "yquant/market/mock_quote_source.h"
#include "yquant/market/pipeline.h"
#include "yquant/market/shm_broadcast.h"

namespace yquant::market {

// Wraps the standalone generator and emits every configured instrument.
// Poll and Close are serialized by the owning daemon thread.
class MockSource final : public Source {
 public:
  MockSource() = default;
  MockSource(const MockSource&) = delete;
  MockSource& operator=(const MockSource&) = delete;

  [[nodiscard]] base::Status Init(const market::MockQuoteConfig& config,
                                  uint64_t now) noexcept;
  [[nodiscard]] PollResult Poll(uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override;

 private:
  market::MockQuoteSource source_;
};

// Attaches once at the live cursor. A gap, generation replacement, stopped or
// stale producer, or corrupt quote is terminal: recovery belongs to the host.
// Refresh is a control-path operation performed at most once per 250 ms.
class ShmSource final : public Source {
 public:
  ShmSource() = default;
  ShmSource(const ShmSource&) = delete;
  ShmSource& operator=(const ShmSource&) = delete;

  [[nodiscard]] base::Status Init(const market::Endpoint& endpoint) noexcept;
  [[nodiscard]] PollResult Poll(uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Records the first terminal error and leaves future polls in that state.
  PollResult Fail(base::Status status, const char* detail) noexcept;

  market::ShmReader reader_;
  market::EventBuffer buffer_;
  market::RefreshResult refreshed_;
  uint64_t next_refresh_ = 0;
  base::Status failure_;
  bool active_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::market

#endif  // YQUANT_MARKET_SOURCES_H_
