#include "marketd/runner.h"

#include <cstdio>
#include <new>
#include <thread>

#include "marketd/adapters.h"
#include "marketd/recording.h"
#include "marketd/replay_source.h"
#include "marketd/tcp_transport.h"
#include "yquant/md/quote_codec.h"

namespace yquant::marketd {

MarketdRunner::~MarketdRunner() noexcept { Close(); }

base::Status MarketdRunner::Fail(base::Status status, const char* stage,
                                 const char* detail) noexcept {
  std::snprintf(error_stage_, sizeof(error_stage_), "%s", stage);
  std::snprintf(error_detail_, sizeof(error_detail_), "%s", detail);
  Close();
  return status;
}

base::Status MarketdRunner::Init(const MarketdConfig& config,
                                 std::uint64_t now) noexcept {
  if (source_)
    return Fail({base::ErrorCode::kInvalidState, {}}, "initialize",
                "pipeline is already initialized");
  base::ConfigError error;
  auto status = ValidateConfig(config, error);
  if (!status.ok()) {
    char detail[768];
    std::snprintf(detail, sizeof(detail), "%s: %s", error.field_path,
                  error.message);
    return Fail(status, "config", detail);
  }
  event_count_ = 0;
  error_stage_[0] = '\0';
  error_detail_[0] = '\0';
  if (config.source.adapter == Adapter::kMock) {
    auto* adapter = new (std::nothrow) MockSource;
    if (adapter != nullptr) {
      source_.reset(adapter);
      status = adapter->Init(config.source.mock, now);
    }
  } else if (config.source.adapter == Adapter::kShm) {
    auto* adapter = new (std::nothrow) ShmSource;
    if (adapter != nullptr) {
      source_.reset(adapter);
      status = adapter->Init(
          {config.source.runtime_directory, config.source.stream});
    }
  } else if (config.source.adapter == Adapter::kReplay) {
    auto* adapter = new (std::nothrow) ReplaySource;
    if (adapter != nullptr) {
      source_.reset(adapter);
      status = adapter->Init(config.source.replay);
    }
  } else {
    auto* adapter = new (std::nothrow) TcpSource;
    if (adapter != nullptr) {
      source_.reset(adapter);
      status = adapter->Init(config.source.tcp);
    }
  }
  if (!source_)
    return Fail({base::ErrorCode::kResourceExhausted, {}}, "source_init",
                "source allocation failed");
  if (!status.ok()) return Fail(status, "source_init", source_->error_detail());
  for (std::size_t i = 0; i < config.publisher_count; ++i) {
    const auto& output = config.publishers[i];
    if (output.adapter == Adapter::kShm) {
      auto* adapter = new (std::nothrow) ShmPublisherAdapter;
      if (adapter != nullptr) {
        publishers_[i].reset(adapter);
        status = adapter->Init({{output.runtime_directory, output.stream},
                                output.capacity,
                                output.max_payload_size});
      }
    } else if (output.adapter == Adapter::kRecord) {
      auto* adapter = new (std::nothrow) RecordPublisher;
      if (adapter != nullptr) {
        publishers_[i].reset(adapter);
        status = adapter->Init(output.record);
      }
    } else {
      auto* adapter = new (std::nothrow) TcpPublisher;
      if (adapter != nullptr) {
        publishers_[i].reset(adapter);
        status = adapter->Init(output.tcp, output.max_clients);
      }
    }
    // Count each allocated adapter so failed initialization is cleaned too.
    publisher_count_ = i + 1;
    if (!publishers_[i])
      return Fail({base::ErrorCode::kResourceExhausted, {}}, "publisher_init",
                  "publisher allocation failed");
    if (!status.ok())
      return Fail(status, "publisher_init", publishers_[i]->error_detail());
  }
  return {};
}

base::Status MarketdRunner::Run(const strategyd::MonotonicClock& clock,
                                strategyd::EventLoopEnvironment& environment,
                                std::uint64_t max_events) noexcept {
  if (!source_)
    return Fail({base::ErrorCode::kInvalidState, {}}, "run",
                "pipeline is not initialized");
  for (;;) {
    bool stop = false;
    auto status = environment.CheckStop(stop);
    if (!status.ok())
      return Fail(status, "termination", "cannot check termination signals");
    if (stop || (max_events != 0 && event_count_ >= max_events)) {
      return Finish();
    }
    const auto now = clock.Now();
    for (std::size_t i = 0; i < publisher_count_; ++i) {
      status = publishers_[i]->Service(now);
      if (!status.ok())
        return Fail(status, "publisher_service",
                    publishers_[i]->error_detail());
    }
    std::size_t batch_count = 0;
    for (; batch_count < 64; ++batch_count) {
      model::QuoteTick quote;
      const auto result = source_->Poll(clock.Now(), quote);
      if (result.state == PollState::kError)
        return Fail(result.status, "source_poll", source_->error_detail());
      if (result.state == PollState::kIdle) break;
      if (result.state == PollState::kEnd) return Finish();
      status = md::ValidateQuote(quote);
      if (!status.ok())
        return Fail(status, "source_poll", "source produced an invalid quote");
      for (std::size_t i = 0; i < publisher_count_; ++i) {
        status = publishers_[i]->Publish(quote);
        if (!status.ok())
          return Fail(status, "publish", publishers_[i]->error_detail());
      }
      ++event_count_;
      if (max_events != 0 && event_count_ >= max_events) break;
    }
    if (batch_count == 0) std::this_thread::yield();
  }
}

void MarketdRunner::Close() noexcept { (void)Finish(); }

base::Status MarketdRunner::Finish() noexcept {
  if (source_) source_->Close();
  source_.reset();
  base::Status first_error;
  for (std::size_t i = 0; i < publisher_count_; ++i) {
    if (publishers_[i]) {
      const auto status = publishers_[i]->Finish();
      if (first_error.ok() && !status.ok()) {
        first_error = status;
        if (error_stage_[0] == '\0') {
          std::snprintf(error_stage_, sizeof(error_stage_), "publisher_finish");
          std::snprintf(error_detail_, sizeof(error_detail_), "%s",
                        publishers_[i]->error_detail());
        }
      }
    }
    publishers_[i].reset();
  }
  publisher_count_ = 0;
  return first_error;
}

}  // namespace yquant::marketd
