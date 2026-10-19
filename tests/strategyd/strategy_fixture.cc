#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

// Each target selects a distinct plugin failure, exercising the real loader.
#ifndef YQUANT_TEST_PLUGIN_MODE
#define YQUANT_TEST_PLUGIN_MODE 0
#endif

extern "C" int YQuantTestDependency() noexcept;

namespace {

using yquant::base::ErrorCode;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyPlugin;

void Emit(char event) noexcept {
  const char* value = std::getenv("YQUANT_TEST_EVENT_FD");
  if (value == nullptr) {
    return;
  }
  const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
  while (write(fd, &event, 1) == -1 && errno == EINTR) {
  }
}

struct ModuleLifetime {
  ~ModuleLifetime() { Emit('U'); }
};

ModuleLifetime module_lifetime;

class FixtureConfig final : public StrategyConfig {
 public:
  bool values_valid() const noexcept {
    return quote_qty_ > 0 && quote_qty_ <= 100 &&
           large_value_ == 9007199254740993LL &&
           std::strcmp(label_, "owned label") == 0;
  }

 protected:
  ErrorCode LoadParameters(std::string_view json,
                           ConfigError* error) noexcept override {
    Emit('P');
    if constexpr (YQUANT_TEST_PLUGIN_MODE != 13) {
      return StrategyConfig::LoadParameters(json, error);
    }
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    simdjson::dom::object object;
    if (parser.parse(json.data(), json.size()).get(root) ||
        root.get_object().get(object)) {
      error->Set("", "expected an object");
      return ErrorCode::kInvalidArgument;
    }
    bool has_qty = false;
    bool has_label = false;
    bool has_large = false;
    std::int64_t quote_qty = 0;
    std::int64_t large_value = 0;
    std::string_view label;
    for (const auto field : object) {
      if (field.key == "quote_qty") {
        if (field.value.get_int64().get(quote_qty) || quote_qty <= 0 ||
            quote_qty > 100) {
          error->Set("quote_qty", "expected an integer from 1 to 100");
          return ErrorCode::kInvalidArgument;
        }
        has_qty = true;
      } else if (field.key == "large_value") {
        if (field.value.get_int64().get(large_value) ||
            large_value != 9007199254740993LL) {
          error->Set("large_value", "expected exact integer 9007199254740993");
          return ErrorCode::kInvalidArgument;
        }
        has_large = true;
      } else if (field.key == "label") {
        if (field.value.get_string().get(label) || label.empty() ||
            label.size() >= sizeof(label_) ||
            label.find('\0') != std::string_view::npos) {
          error->Set("label", "expected a nonempty string of at most 63 bytes");
          return ErrorCode::kInvalidArgument;
        }
        has_label = true;
      } else {
        error->Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
    }
    if (!has_qty || !has_label || !has_large) {
      error->Set(!has_qty     ? "quote_qty"
                 : !has_label ? "label"
                              : "large_value",
                 "required parameter missing");
      return ErrorCode::kInvalidArgument;
    }
    quote_qty_ = quote_qty;
    large_value_ = large_value;
    std::memcpy(label_, label.data(), label.size());
    label_[label.size()] = '\0';
    return ErrorCode::kOk;
  }

 private:
  std::int64_t quote_qty_ = 0;
  std::int64_t large_value_ = 0;
  char label_[64] = {};
};

class FixtureStrategy final : public Strategy {
 public:
  FixtureConfig& config() noexcept override { return config_; }

  ErrorCode OnStart() noexcept override {
    if (!config_.ready() || config_.strategy_id().empty()) {
      return ErrorCode::kInvalidState;
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 13) {
      if (!config_.values_valid()) {
        return ErrorCode::kInvalidState;
      }
    }
    Emit('S');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 12) {
      if (YQuantTestDependency() != 1) {
        return ErrorCode::kInternalError;
      }
    }
    const char* value = std::getenv("YQUANT_TEST_START_RELEASE_FD");
    if (value != nullptr) {
      const int fd = static_cast<int>(std::strtol(value, nullptr, 10));
      char release = 0;
      while (read(fd, &release, 1) == -1 && errno == EINTR) {
      }
    }
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 9) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }

  ErrorCode OnStop() noexcept override {
    Emit('T');
    if constexpr (YQUANT_TEST_PLUGIN_MODE == 10) {
      return ErrorCode::kInvalidArgument;
    }
    return ErrorCode::kOk;
  }

 private:
  FixtureConfig config_;
};

ErrorCode Create(Strategy** strategy) noexcept {
  Emit('C');
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 6) {
    *strategy = nullptr;
    return ErrorCode::kInvalidArgument;
  }
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 7) {
    *strategy = nullptr;
    return ErrorCode::kOk;
  }
  *strategy = new (std::nothrow) FixtureStrategy;
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 8) {
    return ErrorCode::kInvalidArgument;
  }
  return *strategy != nullptr ? ErrorCode::kOk : ErrorCode::kResourceExhausted;
}

void Destroy(Strategy* strategy) noexcept {
  delete strategy;
  Emit('D');
}

[[maybe_unused]] const StrategyPlugin plugin = {
    YQUANT_TEST_PLUGIN_MODE == 2
        ? yquant::strategy::kStrategyPluginAbiVersion + 1
        : yquant::strategy::kStrategyPluginAbiVersion,
    YQUANT_TEST_PLUGIN_MODE == 3
        ? 0U
        : static_cast<std::uint32_t>(sizeof(StrategyPlugin)),
    YQUANT_TEST_PLUGIN_MODE == 4 ? nullptr : &Create,
    YQUANT_TEST_PLUGIN_MODE == 5 ? nullptr : &Destroy,
};

}  // namespace

#if YQUANT_TEST_PLUGIN_MODE != 1
extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  if constexpr (YQUANT_TEST_PLUGIN_MODE == 11) {
    return nullptr;
  }
  return &plugin;
}
#endif
