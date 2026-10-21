#include "marketd/config.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <simdjson.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <span>
#include <string_view>
#include <utility>

#include "strategyd/json_config_document.h"
#include "yquant/md/quote_codec.h"
#include "yquant/md/shm_broadcast.h"

namespace yquant::marketd {
namespace {

// Owns a bounded diagnostic path for one adapter field.
struct FieldPath {
  char text[256] = {};
  FieldPath(bool source, std::size_t index, std::string_view field) noexcept {
    if (source) {
      std::snprintf(text, sizeof(text), "source.%.*s",
                    static_cast<int>(field.size()), field.data());
    } else {
      std::snprintf(text, sizeof(text), "publishers[%zu].%.*s", index,
                    static_cast<int>(field.size()), field.data());
    }
  }
};

// Stores an owned schema error; no parser or input references survive.
base::Status Invalid(std::string_view path, std::string_view reason,
                     strategy::ConfigError& error) noexcept {
  error.Set(path, reason);
  return {base::ErrorCode::kInvalidArgument, {}};
}

// Preserves allocation failures instead of reporting them as malformed JSON.
base::Status JsonError(simdjson::error_code code, std::string_view path,
                       strategy::ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(code));
  return {code == simdjson::MEMALLOC ? base::ErrorCode::kResourceExhausted
                                     : base::ErrorCode::kInvalidArgument,
          {}};
}

// Copies a nonempty decoded string without truncation or embedded NUL.
base::Status ReadText(simdjson::ondemand::value value, char* out,
                      std::size_t capacity, std::string_view path,
                      strategy::ConfigError& error) noexcept {
  std::string_view text;
  const auto code = value.get_string().get(text);
  if (code) return JsonError(code, path, error);
  if (text.empty() || text.size() >= capacity ||
      text.find('\0') != std::string_view::npos) {
    return Invalid(path, "empty, overlong or NUL-containing string", error);
  }
  std::memcpy(out, text.data(), text.size());
  out[text.size()] = '\0';
  return {};
}

// Reads unsigned integers exactly and checks the destination width.
base::Status ReadSize(simdjson::ondemand::value value, std::size_t& out,
                      std::string_view path,
                      strategy::ConfigError& error) noexcept {
  std::uint64_t number = 0;
  const auto code = value.get_uint64().get(number);
  if (code) return JsonError(code, path, error);
  if (number > SIZE_MAX) return Invalid(path, "integer is too large", error);
  out = number;
  return {};
}

// Parses every adapter field before applying its adapter-specific allowed and
// required field masks, so JSON key order never changes the schema.
base::Status ParseAdapter(simdjson::ondemand::value value, bool source,
                          std::size_t index, SourceConfig& input,
                          PublisherConfig& output, char* mock_path,
                          strategy::ConfigError& error) noexcept {
  constexpr std::string_view kFields[] = {
      "adapter",        "config",        "runtime_directory",
      "stream",         "capacity",      "max_payload_size",
      "address",        "port",          "max_clients",
      "path",           "format",        "speed",
      "max_records",    "source_id",     "instrument_catalog",
      "queue_capacity", "start_delay_ms"};
  simdjson::ondemand::object object;
  auto code = value.get_object().get(object);
  if (code) return JsonError(code, FieldPath(source, index, "").text, error);
  std::uint32_t seen = 0;
  char adapter[16] = {};
  char format[24] = {};
  char* runtime = source ? input.runtime_directory : output.runtime_directory;
  char* stream = source ? input.stream : output.stream;
  auto& tcp = source ? input.tcp : output.tcp;
  for (auto result : object) {
    simdjson::ondemand::field field;
    code = std::move(result).get(field);
    if (code) return JsonError(code, "$", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "$", error);
    const FieldPath path(source, index, key);
    std::size_t position = 0;
    while (position < std::size(kFields) && key != kFields[position])
      ++position;
    if (position == std::size(kFields))
      return Invalid(path.text, "unknown adapter field", error);
    seen |= 1U << position;
    base::Status status;
    switch (position) {
      case 0:
        status =
            ReadText(field.value(), adapter, sizeof(adapter), path.text, error);
        break;
      case 1:
        status = ReadText(field.value(), mock_path, kMaxRuntimePathBytes + 1,
                          path.text, error);
        break;
      case 2:
        status = ReadText(field.value(), runtime, kMaxRuntimePathBytes + 1,
                          path.text, error);
        break;
      case 3:
        status = ReadText(field.value(), stream, kMaxStreamBytes + 1, path.text,
                          error);
        break;
      case 4:
        status = ReadSize(field.value(), output.capacity, path.text, error);
        break;
      case 5:
        status =
            ReadSize(field.value(), output.max_payload_size, path.text, error);
        break;
      case 6:
        status = ReadText(field.value(), tcp.address, sizeof(tcp.address),
                          path.text, error);
        break;
      case 7: {
        std::size_t port = 0;
        status = ReadSize(field.value(), port, path.text, error);
        if (status.ok() && (port == 0 || port > UINT16_MAX))
          status = Invalid(path.text, "port must be from 1 to 65535", error);
        if (status.ok()) tcp.port = port;
        break;
      }
      case 8:
        status = ReadSize(field.value(), output.max_clients, path.text, error);
        break;
      case 9:
        status = ReadText(field.value(),
                          source ? input.replay.path : output.record.path,
                          kMaxDataPathBytes + 1, path.text, error);
        break;
      case 10:
        status =
            ReadText(field.value(), format, sizeof(format), path.text, error);
        break;
      case 11: {
        const auto number_error =
            field.value().get_double().get(input.replay.speed);
        if (number_error) status = JsonError(number_error, path.text, error);
        break;
      }
      case 12:
        status =
            ReadSize(field.value(), input.replay.max_records, path.text, error);
        break;
      case 13: {
        std::size_t id = 0;
        status = ReadSize(field.value(), id, path.text, error);
        if (status.ok() && (id == 0 || id > UINT32_MAX))
          status = Invalid(path.text, "must be from 1 to 4294967295", error);
        if (status.ok()) input.replay.source_id.value = id;
        break;
      }
      case 14:
        status =
            ReadText(field.value(), input.replay.instrument_catalog,
                     sizeof(input.replay.instrument_catalog), path.text, error);
        break;
      case 15:
        status = ReadSize(field.value(), output.record.queue_capacity,
                          path.text, error);
        break;
      case 16: {
        std::size_t delay = 0;
        status = ReadSize(field.value(), delay, path.text, error);
        if (status.ok() && delay > 60000)
          status = Invalid(path.text, "must be from 0 to 60000 ms", error);
        if (status.ok()) input.replay.start_delay_ns = delay * 1000000ULL;
        break;
      }
    }
    if (!status.ok()) return status;
  }
  Adapter selected;
  std::uint32_t required;
  std::uint32_t allowed;
  if (std::strcmp(adapter, "mock") == 0 && source) {
    selected = Adapter::kMock;
    required = allowed = (1U << 0) | (1U << 1);
  } else if (std::strcmp(adapter, "shm") == 0) {
    selected = Adapter::kShm;
    required = (1U << 0) | (1U << 2) | (1U << 3);
    allowed = required | (source ? 0 : (1U << 4) | (1U << 5));
  } else if (std::strcmp(adapter, "tcp") == 0) {
    selected = Adapter::kTcp;
    required = (1U << 0) | (1U << 6) | (1U << 7);
    allowed = required | (source ? 0 : (1U << 8));
  } else if (std::strcmp(adapter, "replay") == 0 && source) {
    selected = Adapter::kReplay;
    required = (1U << 0) | (1U << 9) | (1U << 10);
    allowed = required | (1U << 11) | (1U << 12) | (1U << 16);
    if (std::strcmp(format, "dolphindb_csv") == 0) {
      input.replay.format = ReplayFormat::kDolphinDbCsv;
      required |= (1U << 13) | (1U << 14);
      allowed |= (1U << 13) | (1U << 14);
    } else if (std::strcmp(format, "recording") == 0) {
      input.replay.format = ReplayFormat::kRecording;
    } else {
      return Invalid("source.format", "unsupported or missing replay format",
                     error);
    }
  } else if (std::strcmp(adapter, "record") == 0 && !source) {
    selected = Adapter::kRecord;
    required = (1U << 0) | (1U << 9);
    allowed = required | (1U << 15);
  } else {
    return Invalid(FieldPath(source, index, "adapter").text,
                   "unsupported or missing adapter", error);
  }
  for (std::size_t i = 0; i < std::size(kFields); ++i) {
    if ((required & (1U << i)) != 0 && (seen & (1U << i)) == 0)
      return Invalid(FieldPath(source, index, kFields[i]).text,
                     "required field is missing", error);
    if ((seen & (1U << i)) != 0 && (allowed & (1U << i)) == 0)
      return Invalid(FieldPath(source, index, kFields[i]).text,
                     "field is not supported by this adapter", error);
  }
  if (source)
    input.adapter = selected;
  else
    output.adapter = selected;
  return {};
}

// Applies the daemon schema to already validated UTF-8 JSON.
base::Status Parse(const strategyd::JsonConfigDocument& source,
                   MarketdConfig& out, char* mock_path,
                   strategy::ConfigError& error) noexcept {
  simdjson::ondemand::parser parser;
  auto code =
      parser.allocate(source.source().size(), strategyd::kMaxConfigDepth + 4);
  if (code) return JsonError(code, "$", error);
  simdjson::ondemand::document document;
  code = parser.iterate(source.source(), source.capacity()).get(document);
  if (code) return JsonError(code, "$", error);
  simdjson::ondemand::object root;
  code = document.get_object().get(root);
  if (code) return JsonError(code, "$", error);
  bool has_source = false;
  bool has_publishers = false;
  for (auto result : root) {
    simdjson::ondemand::field field;
    code = std::move(result).get(field);
    if (code) return JsonError(code, "$", error);
    std::string_view key;
    code = field.unescaped_key().get(key);
    if (code) return JsonError(code, "$", error);
    if (key == "source") {
      PublisherConfig ignored;
      const auto status = ParseAdapter(field.value(), true, 0, out.source,
                                       ignored, mock_path, error);
      if (!status.ok()) return status;
      has_source = true;
    } else if (key == "publishers") {
      simdjson::ondemand::array array;
      code = field.value().get_array().get(array);
      if (code) return JsonError(code, key, error);
      for (auto entry : array) {
        if (out.publisher_count == kMaxPublishers)
          return Invalid(key, "at most 8 publishers are allowed", error);
        simdjson::ondemand::value item;
        code = std::move(entry).get(item);
        if (code) return JsonError(code, key, error);
        SourceConfig ignored;
        char ignored_path[kMaxRuntimePathBytes + 1] = {};
        const auto status = ParseAdapter(
            item, false, out.publisher_count, ignored,
            out.publishers[out.publisher_count], ignored_path, error);
        if (!status.ok()) return status;
        ++out.publisher_count;
      }
      has_publishers = true;
    } else {
      return Invalid(key, "unknown root field", error);
    }
  }
  if (!has_source) return Invalid("source", "required field is missing", error);
  if (!has_publishers)
    return Invalid("publishers", "required field is missing", error);
  return {};
}

// Checks fixed strings before any filesystem or socket API sees them.
base::Status ValidateShm(const char* directory, const char* stream, bool source,
                         std::size_t index,
                         strategy::ConfigError& error) noexcept {
  if (strnlen(directory, kMaxRuntimePathBytes + 1) > kMaxRuntimePathBytes ||
      directory[0] != '/') {
    return Invalid(FieldPath(source, index, "runtime_directory").text,
                   "must be a bounded absolute path", error);
  }
  const auto length = strnlen(stream, kMaxStreamBytes + 1);
  if (length == 0 || length > kMaxStreamBytes ||
      std::strcmp(stream, ".") == 0 || std::strcmp(stream, "..") == 0) {
    return Invalid(FieldPath(source, index, "stream").text,
                   "invalid stream name", error);
  }
  for (std::size_t i = 0; i < length; ++i) {
    const char c = stream[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
      return Invalid(FieldPath(source, index, "stream").text,
                     "stream uses unsupported characters", error);
    }
  }
  const char first = stream[0];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
        (first >= '0' && first <= '9'))) {
    return Invalid(FieldPath(source, index, "stream").text,
                   "stream must start with an ASCII letter or digit", error);
  }
  const auto validated =
      md::ValidatePublishConfig({{directory, stream}, 2, md::kQuoteFrameSize});
  if (!validated.ok()) {
    return Invalid(FieldPath(source, index, "runtime_directory").text,
                   "unsupported shared-memory endpoint", error);
  }
  return {};
}

// Numeric IPv4 only: DNS resolution cannot enter initialization or polling.
base::Status ValidateTcp(const TcpEndpoint& tcp, bool source, std::size_t index,
                         strategy::ConfigError& error) noexcept {
  in_addr address = {};
  if (strnlen(tcp.address, sizeof(tcp.address)) >= sizeof(tcp.address) ||
      inet_pton(AF_INET, tcp.address, &address) != 1 ||
      (source && address.s_addr == htonl(INADDR_ANY))) {
    return Invalid(FieldPath(source, index, "address").text,
                   "must be a numeric IPv4 address", error);
  }
  if (tcp.port == 0)
    return Invalid(FieldPath(source, index, "port").text,
                   "port must be from 1 to 65535", error);
  return {};
}

// Canonicalizes existing parents so aliases cannot bypass feedback checks.
std::filesystem::path Normalized(const char* path) noexcept {
  std::error_code error;
  auto result = std::filesystem::weakly_canonical(path, error);
  return error ? std::filesystem::path(path).lexically_normal() : result;
}

// File paths are resolved by the JSON loader; typed callers provide absolute
// paths. Existence and read/write access are checked only by adapter Init.
base::Status ValidateFilePath(const char* path, bool source, std::size_t index,
                              strategy::ConfigError& error) noexcept {
  if (strnlen(path, kMaxDataPathBytes + 1) > kMaxDataPathBytes ||
      path[0] != '/')
    return Invalid(FieldPath(source, index, "path").text,
                   "must be a bounded absolute file path", error);
  const auto name = std::filesystem::path(path).filename();
  if (name.empty() || name == "." || name == "..")
    return Invalid(FieldPath(source, index, "path").text, "must name a file",
                   error);
  return {};
}

// Resolves file adapters without opening their data files.
base::Status ResolveFilePath(char* value, const char* config_path, bool source,
                             std::size_t index,
                             strategy::ConfigError& error) noexcept {
  auto resolved = std::filesystem::path(value);
  if (resolved.is_relative()) {
    std::error_code filesystem_error;
    resolved = std::filesystem::absolute(
        std::filesystem::path(config_path).parent_path() / resolved,
        filesystem_error);
    if (filesystem_error) {
      error.Set(FieldPath(source, index, "path").text,
                "cannot resolve relative path");
      return {base::ErrorCode::kIoError, filesystem_error};
    }
  }
  if (resolved.native().size() > kMaxDataPathBytes)
    return Invalid(FieldPath(source, index, "path").text,
                   "resolved path is too long", error);
  std::snprintf(value, kMaxDataPathBytes + 1, "%s", resolved.c_str());
  return {};
}

// Tests one logical SHM endpoint, independent of pathname spelling.
bool SameShm(const char* left_directory, const char* left_stream,
             const char* right_directory, const char* right_stream) noexcept {
  return std::strcmp(left_stream, right_stream) == 0 &&
         Normalized(left_directory) == Normalized(right_directory);
}

// Wildcard binds overlap every local address for duplicate-listener checks.
bool SameTcp(const TcpEndpoint& left, const TcpEndpoint& right) noexcept {
  return left.port == right.port &&
         (std::strcmp(left.address, right.address) == 0 ||
          std::strcmp(left.address, "0.0.0.0") == 0 ||
          std::strcmp(right.address, "0.0.0.0") == 0);
}

// Wildcard listeners receive only traffic addressed to this host. A relay can
// therefore reuse its remote upstream's port without feeding back into itself.
bool IsLocalAddress(const char* text) noexcept {
  in_addr address = {};
  if (inet_pton(AF_INET, text, &address) != 1) return false;
  if ((ntohl(address.s_addr) >> 24) == 127) return true;
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) return false;
  bool local = false;
  for (auto* item = interfaces; item != nullptr; item = item->ifa_next) {
    if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET)
      continue;
    const auto* current = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
    if (current->sin_addr.s_addr == address.s_addr) {
      local = true;
      break;
    }
  }
  freeifaddrs(interfaces);
  return local;
}

// Detects immediate TCP feedback separately from listener-bind overlap.
bool TcpFeedback(const TcpEndpoint& input, const TcpEndpoint& output) noexcept {
  if (input.port != output.port) return false;
  if (std::strcmp(input.address, output.address) == 0) return true;
  return std::strcmp(output.address, "0.0.0.0") == 0 &&
         IsLocalAddress(input.address);
}

}  // namespace

base::Status ValidateConfig(const MarketdConfig& config,
                            strategy::ConfigError& error) noexcept {
  error.Clear();
  if (config.publisher_count == 0 || config.publisher_count > kMaxPublishers)
    return Invalid("publishers", "must contain from 1 to 8 publishers", error);
  base::Status status;
  switch (config.source.adapter) {
    case Adapter::kMock:
      if (config.source.mock.adapter != strategyd::MarketDataAdapter::kMock)
        return Invalid("source.config.adapter", "must be mock", error);
      status = strategyd::ValidateMarketDataConfig(config.source.mock, error);
      break;
    case Adapter::kShm:
      status = ValidateShm(config.source.runtime_directory,
                           config.source.stream, true, 0, error);
      break;
    case Adapter::kTcp:
      status = ValidateTcp(config.source.tcp, true, 0, error);
      break;
    case Adapter::kReplay: {
      const auto& replay = config.source.replay;
      status = ValidateFilePath(replay.path, true, 0, error);
      if (!status.ok()) return status;
      if (replay.format != ReplayFormat::kRecording &&
          replay.format != ReplayFormat::kDolphinDbCsv)
        return Invalid("source.format", "unsupported replay format", error);
      if (!std::isfinite(replay.speed) || replay.speed < 0)
        return Invalid("source.speed", "must be finite and nonnegative", error);
      if (replay.start_delay_ns > 60000000000ULL)
        return Invalid("source.start_delay_ms", "must be from 0 to 60000 ms",
                       error);
      if (replay.max_records == 0 || replay.max_records > kMaxReplayRecords)
        return Invalid("source.max_records", "must be from 1 to 2000000",
                       error);
      if (replay.format == ReplayFormat::kDolphinDbCsv) {
        if (replay.source_id.value == 0)
          return Invalid("source.source_id", "must be nonzero", error);
        if (replay.instrument_catalog[0] != '/' ||
            std::memchr(replay.instrument_catalog, '\0',
                        sizeof(replay.instrument_catalog)) == nullptr)
          return Invalid("source.instrument_catalog",
                         "must be a bounded absolute catalog path", error);
      }
      break;
    }
    default:
      return Invalid("source.adapter", "unsupported adapter", error);
  }
  if (!status.ok()) return status;
  for (std::size_t i = 0; i < config.publisher_count; ++i) {
    const auto& output = config.publishers[i];
    if (output.adapter == Adapter::kShm) {
      status =
          ValidateShm(output.runtime_directory, output.stream, false, i, error);
      if (!status.ok()) return status;
      if (output.capacity < 2 ||
          (output.capacity & (output.capacity - 1)) != 0 ||
          output.capacity > (1U << 24)) {
        return Invalid(FieldPath(false, i, "capacity").text,
                       "must be a power of two from 2 to 16777216", error);
      }
      if (output.max_payload_size < md::kQuoteFrameSize ||
          output.max_payload_size > md::kMaxEventPayloadSize) {
        return Invalid(FieldPath(false, i, "max_payload_size").text,
                       "must be from 80 to 4096 bytes", error);
      }
      const auto layout_status =
          md::ValidatePublishConfig({{output.runtime_directory, output.stream},
                                     output.capacity,
                                     output.max_payload_size});
      if (!layout_status.ok()) {
        return Invalid(FieldPath(false, i, "capacity").text,
                       "shared-memory layout exceeds platform limits", error);
      }
      if (config.source.adapter == Adapter::kShm &&
          SameShm(config.source.runtime_directory, config.source.stream,
                  output.runtime_directory, output.stream)) {
        return Invalid(FieldPath(false, i, "stream").text,
                       "source and publisher cannot be the same stream", error);
      }
    } else if (output.adapter == Adapter::kTcp) {
      status = ValidateTcp(output.tcp, false, i, error);
      if (!status.ok()) return status;
      if (output.max_clients == 0 || output.max_clients > kMaxTcpClients)
        return Invalid(FieldPath(false, i, "max_clients").text,
                       "must be from 1 to 16", error);
      if (config.source.adapter == Adapter::kTcp &&
          TcpFeedback(config.source.tcp, output.tcp)) {
        return Invalid(FieldPath(false, i, "port").text,
                       "source and publisher cannot be the same endpoint",
                       error);
      }
    } else if (output.adapter == Adapter::kRecord) {
      status = ValidateFilePath(output.record.path, false, i, error);
      if (!status.ok()) return status;
      const auto capacity = output.record.queue_capacity;
      if (capacity < 256 || capacity > kMaxRecordingQueueBytes ||
          (capacity & (capacity - 1)) != 0)
        return Invalid(FieldPath(false, i, "queue_capacity").text,
                       "must be a power of two from 256 to 67108864 bytes",
                       error);
      if (config.source.adapter == Adapter::kReplay &&
          Normalized(config.source.replay.path) ==
              Normalized(output.record.path))
        return Invalid(FieldPath(false, i, "path").text,
                       "source and recording cannot be the same file", error);
    } else {
      return Invalid(FieldPath(false, i, "adapter").text,
                     "unsupported publisher adapter", error);
    }
    for (std::size_t j = 0; j < i; ++j) {
      const auto& other = config.publishers[j];
      if (output.adapter != other.adapter) continue;
      bool duplicate = false;
      if (output.adapter == Adapter::kShm)
        duplicate = SameShm(output.runtime_directory, output.stream,
                            other.runtime_directory, other.stream);
      else if (output.adapter == Adapter::kTcp)
        duplicate = SameTcp(output.tcp, other.tcp);
      else if (output.adapter == Adapter::kRecord)
        duplicate =
            Normalized(output.record.path) == Normalized(other.record.path);
      if (duplicate)
        return Invalid(FieldPath(false, i, "adapter").text,
                       "duplicate publisher endpoint", error);
    }
  }
  return {};
}

base::Status ConfigLoader::Load(const char* path) noexcept {
  config_ = {};
  error_.Clear();
  strategyd::JsonConfigDocument document;
  auto status = document.Load(path);
  if (!status.ok()) {
    error_ = document.error();
    return status;
  }
  MarketdConfig parsed;
  char mock_path[kMaxRuntimePathBytes + 1] = {};
  status = Parse(document, parsed, mock_path, error_);
  if (!status.ok()) return status;
  if (parsed.source.adapter == Adapter::kMock) {
    std::filesystem::path resolved(mock_path);
    if (resolved.is_relative())
      resolved = std::filesystem::path(path).parent_path() / resolved;
    if (resolved.native().size() > kMaxRuntimePathBytes)
      return Invalid("source.config", "resolved path is too long", error_);
    strategyd::MarketDataConfigLoader loader;
    status = loader.Load(resolved.c_str());
    if (!status.ok()) {
      char field[256];
      std::snprintf(field, sizeof(field), "source.config.%.230s",
                    loader.error().field_path);
      error_.Set(field, loader.error().message);
      error_.truncated |= loader.error().truncated;
      return status;
    }
    parsed.source.mock = loader.config();
  } else if (parsed.source.adapter == Adapter::kReplay) {
    status = ResolveFilePath(parsed.source.replay.path, path, true, 0, error_);
    if (!status.ok()) return status;
    if (parsed.source.replay.format == ReplayFormat::kDolphinDbCsv) {
      strategy::ConfigError catalog_error;
      status = strategyd::ResolveInstrumentCatalogPath(
          path, parsed.source.replay.instrument_catalog,
          std::span(parsed.source.replay.instrument_catalog), catalog_error);
      if (!status.ok()) {
        error_.Set("source.instrument_catalog", catalog_error.message);
        error_.truncated |= catalog_error.truncated;
        return status;
      }
    }
  }
  for (std::size_t i = 0; i < parsed.publisher_count; ++i) {
    if (parsed.publishers[i].adapter != Adapter::kRecord) continue;
    status = ResolveFilePath(parsed.publishers[i].record.path, path, false, i,
                             error_);
    if (!status.ok()) return status;
  }
  status = ValidateConfig(parsed, error_);
  if (status.ok()) config_ = parsed;
  return status;
}

}  // namespace yquant::marketd
