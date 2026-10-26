#ifndef YQUANT_MD_PIPELINE_CONFIG_H_
#define YQUANT_MD_PIPELINE_CONFIG_H_

#include <stddef.h>

#include <array>

#include "yquant/base/config_error.h"
#include "yquant/base/status.h"
#include "yquant/md/file_config.h"
#include "yquant/md/mock_quote_config.h"
#include "yquant/md/tcp_transport.h"

namespace yquant::md {

inline constexpr size_t kMaxPublishers = 8;
inline constexpr size_t kMaxRuntimePathBytes = 1023;
inline constexpr size_t kMaxStreamBytes = 64;

enum class Adapter { kMock, kShm, kTcp, kReplay, kRecord };

// Owns initialization data; inactive adapter fields are ignored. Mock metadata
// is loaded before any transport is opened. Paths and names are NUL-terminated.
struct SourceConfig {
  Adapter adapter = Adapter::kMock;
  md::MockQuoteConfig mock;
  char runtime_directory[kMaxRuntimePathBytes + 1] = {};
  char stream[kMaxStreamBytes + 1] = {};
  TcpEndpoint tcp;
  ReplayConfig replay;
};

struct PublisherConfig {
  Adapter adapter = Adapter::kShm;
  char runtime_directory[kMaxRuntimePathBytes + 1] = {};
  char stream[kMaxStreamBytes + 1] = {};
  size_t capacity = 1024;
  size_t max_payload_size = 256;
  TcpEndpoint tcp;
  size_t max_clients = 16;
  RecordConfig record;
};

// Only the first publisher_count entries are active; the daemon owns a single
// source and publishes each accepted event in array order.
struct PipelineConfig {
  SourceConfig source;
  std::array<PublisherConfig, kMaxPublishers> publishers;
  size_t publisher_count = 0;
};

// Validates typed configuration without acquiring output resources. Rejects
// immediate feedback and duplicate outputs. Failure preserves config.
[[nodiscard]] base::Status ValidatePipelineConfig(
    const PipelineConfig& config, base::ConfigError& error) noexcept;

// Strict JSON loader: decoded duplicate/unknown fields, wrong types and
// overlong strings fail. Relative mock, replay, catalog and recording paths
// resolve against the process working directory. Validation does not read
// replay data or the CSV catalog and does not create files.
class PipelineConfigLoader {
 public:
  PipelineConfigLoader() = default;
  PipelineConfigLoader(const PipelineConfigLoader&) = default;
  PipelineConfigLoader& operator=(const PipelineConfigLoader&) = default;

  // Replaces prior configuration. Failure empties config() and owns its
  // diagnostic until the next call. No transport or discovery is created.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const PipelineConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  PipelineConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::md

#endif  // YQUANT_MD_PIPELINE_CONFIG_H_
