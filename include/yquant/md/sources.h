#ifndef YQUANT_MD_SOURCES_H_
#define YQUANT_MD_SOURCES_H_

#include <stdint.h>

#include "yquant/md/mock_quote_config.h"
#include "yquant/md/mock_quote_source.h"
#include "yquant/md/pipeline.h"
#include "yquant/md/shm_broadcast.h"

namespace yquant::md {

// Wraps the standalone generator and emits every configured instrument.
// Poll and Close are serialized by the owning daemon thread.
class MockSource final : public Source {
 public:
  MockSource() = default;
  MockSource(const MockSource&) = delete;
  MockSource& operator=(const MockSource&) = delete;

  [[nodiscard]] base::Status Init(const md::MockQuoteConfig& config,
                                  uint64_t now) noexcept;
  [[nodiscard]] PollResult Poll(uint64_t now,
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
  ShmSource() = default;
  ShmSource(const ShmSource&) = delete;
  ShmSource& operator=(const ShmSource&) = delete;

  [[nodiscard]] base::Status Init(const md::Endpoint& endpoint) noexcept;
  [[nodiscard]] PollResult Poll(uint64_t now,
                                model::QuoteTick& out) noexcept override;
  void Close() noexcept override;
  const char* error_detail() const noexcept override { return error_detail_; }

 private:
  // Records the first terminal error and leaves future polls in that state.
  PollResult Fail(base::Status status, const char* detail) noexcept;

  md::ShmReader reader_;
  md::EventBuffer buffer_;
  md::RefreshResult refreshed_;
  uint64_t next_refresh_ = 0;
  base::Status failure_;
  bool active_ = false;
  char error_detail_[256] = {};
};

}  // namespace yquant::md

#endif  // YQUANT_MD_SOURCES_H_
