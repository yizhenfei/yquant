#ifndef YQUANT_MD_CSV_REPLAY_H_
#define YQUANT_MD_CSV_REPLAY_H_

#include <span>

#include "yquant/base/status.h"
#include "yquant/md/file_config.h"
#include "yquant/md/replay_data.h"

namespace yquant::md {

// Loads a regular DolphinDB snapshot CSV into bounded, owned replay storage.
// Required columns are Date, Time, SecurityID, SecurityIDSource, OfferPX1,
// BidPX1, OfferSize1, and BidSize1; other columns are ignored. Dates/times use
// UTC+08. Loads config.instrument_catalog before reading CSV records; every
// instrument must be defined. Nonnegative prices round to each definition's
// price_decimals, with ties upward. Replay offsets use RecvTime when that
// column exists, otherwise Date + Time. File order is preserved; backward
// times cannot reduce replay offsets. On failure output is unchanged;
// error_detail receives a terminated diagnostic when nonempty. No views into
// the file or configuration survive this call.
[[nodiscard]] base::Status LoadDolphinDbCsv(
    const ReplayConfig& config, ReplayData& output,
    std::span<char> error_detail) noexcept;

}  // namespace yquant::md

#endif  // YQUANT_MD_CSV_REPLAY_H_
