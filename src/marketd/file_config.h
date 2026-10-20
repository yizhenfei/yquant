#ifndef YQUANT_MARKETD_FILE_CONFIG_H_
#define YQUANT_MARKETD_FILE_CONFIG_H_

#include <cstddef>
#include <cstdint>

#include "strategyd/instrument_catalog.h"
#include "yquant/model/identifiers.h"

namespace yquant::marketd {

inline constexpr std::size_t kMaxDataPathBytes = 4095;
inline constexpr std::size_t kMaxReplayRecords = 2000000;
inline constexpr std::size_t kMaxRecordingQueueBytes = 64 * 1024 * 1024;

enum class ReplayFormat { kRecording, kDolphinDbCsv };

// Replay storage is bounded and populated before the pipeline starts. Zero
// speed emits as fast as the host polls; positive speed scales arrival gaps.
// source_id and instrument_catalog apply only to DolphinDB CSV conversion.
struct ReplayConfig {
  char path[kMaxDataPathBytes + 1] = {};
  ReplayFormat format = ReplayFormat::kRecording;
  double speed = 1.0;
  // Delay from the first Poll, allowing local subscribers to attach first.
  std::uint64_t start_delay_ns = 0;
  std::size_t max_records = 1000000;
  model::SourceId source_id{1};
  // Absolute catalog path, loaded only while converting the CSV input.
  char instrument_catalog[strategyd::kMaxInstrumentCatalogPathLength + 1] = {};
};

// The output is created exclusively, never appended to or overwritten.
// queue_capacity is the power-of-two byte capacity of the async SPSC queue.
struct RecordConfig {
  char path[kMaxDataPathBytes + 1] = {};
  std::size_t queue_capacity = 1024 * 1024;
};

}  // namespace yquant::marketd

#endif  // YQUANT_MARKETD_FILE_CONFIG_H_
