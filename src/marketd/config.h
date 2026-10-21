#ifndef YQUANT_MARKETD_CONFIG_H_
#define YQUANT_MARKETD_CONFIG_H_

#include <array>
#include <cstddef>

#include "marketd/file_config.h"
#include "marketd/tcp_transport.h"
#include "yquant/base/config_error.h"
#include "yquant/md/mock_quote_config.h"

namespace yquant::marketd {

inline constexpr std::size_t kMaxPublishers = 8;
inline constexpr std::size_t kMaxRuntimePathBytes = 1023;
inline constexpr std::size_t kMaxStreamBytes = 64;

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
  std::size_t capacity = 1024;
  std::size_t max_payload_size = 256;
  TcpEndpoint tcp;
  std::size_t max_clients = 16;
  RecordConfig record;
};

// Only the first publisher_count entries are active; the daemon owns a single
// source and publishes each accepted event in array order.
struct MarketdConfig {
  SourceConfig source;
  std::array<PublisherConfig, kMaxPublishers> publishers;
  std::size_t publisher_count = 0;
};

// Validates typed configuration without acquiring output resources. Rejects
// immediate feedback and duplicate outputs. Failure preserves config.
[[nodiscard]] base::Status ValidateConfig(const MarketdConfig& config,
                                          base::ConfigError& error) noexcept;

// Strict JSON loader: decoded duplicate/unknown fields, wrong types and
// overlong strings fail. Relative mock, replay, catalog and recording paths
// resolve against the process working directory. Validation does not read
// replay data or the CSV catalog and does not create files.
class ConfigLoader {
 public:
  // Replaces prior configuration. Failure empties config() and owns its
  // diagnostic until the next call. No transport or discovery is created.
  [[nodiscard]] base::Status Load(const char* path) noexcept;
  const MarketdConfig& config() const noexcept { return config_; }
  const base::ConfigError& error() const noexcept { return error_; }

 private:
  MarketdConfig config_;
  base::ConfigError error_;
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_CONFIG_H_
