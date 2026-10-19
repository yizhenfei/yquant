#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson.h"
#include "yquant/base/status.h"
#include "yquant/strategy/strategy.h"
#include "yquant/strategy/strategy_plugin.h"

namespace {

using yquant::base::ErrorCode;
using yquant::strategy::ConfigError;
using yquant::strategy::Strategy;
using yquant::strategy::StrategyConfig;
using yquant::strategy::StrategyPlugin;

constexpr std::size_t kMaxStartupMessageBytes = 128;

ErrorCode ParseError(simdjson::error_code result, std::string_view path,
                     ConfigError& error) noexcept {
  error.Set(path, simdjson::error_message(result));
  return result == simdjson::MEMALLOC ? ErrorCode::kResourceExhausted
                                      : ErrorCode::kInvalidArgument;
}

class LifecycleConfig final : public StrategyConfig {
 public:
  std::string_view startup_message() const noexcept {
    return {startup_message_, startup_message_size_};
  }

 protected:
  ErrorCode LoadParameters(std::string_view parameters_json,
                           ConfigError& error) noexcept override {
    simdjson::padded_string input(parameters_json);
    if (input.data() == nullptr) {
      error.Set("", "could not allocate parameter input");
      return ErrorCode::kResourceExhausted;
    }
    simdjson::dom::parser parser;
    if (const auto result = parser.allocate(input.size());
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::element root;
    if (const auto result = parser.parse(input).get(root);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }
    simdjson::dom::object object;
    if (const auto result = root.get_object().get(object);
        result != simdjson::SUCCESS) {
      return ParseError(result, "", error);
    }

    std::string_view startup_message;
    bool has_startup_message = false;
    for (const auto field : object) {
      if (field.key != "startup_message") {
        error.Set(field.key, "unknown parameter");
        return ErrorCode::kInvalidArgument;
      }
      if (has_startup_message) {
        error.Set(field.key, "duplicate parameter");
        return ErrorCode::kInvalidArgument;
      }
      if (const auto result = field.value.get_string().get(startup_message);
          result != simdjson::SUCCESS) {
        return ParseError(result, field.key, error);
      }
      has_startup_message = true;
    }
    if (!has_startup_message) {
      error.Set("startup_message", "required parameter is missing");
      return ErrorCode::kInvalidArgument;
    }
    if (startup_message.empty() ||
        startup_message.size() > kMaxStartupMessageBytes ||
        startup_message.find('\0') != std::string_view::npos) {
      error.Set("startup_message", "must contain 1 to 128 bytes without NUL");
      return ErrorCode::kInvalidArgument;
    }

    // Keep decoded bytes in the config: JSON storage is gone before OnStart.
    std::memcpy(startup_message_, startup_message.data(),
                startup_message.size());
    startup_message_size_ = startup_message.size();
    return ErrorCode::kOk;
  }

 private:
  char startup_message_[kMaxStartupMessageBytes] = {};
  std::size_t startup_message_size_ = 0;
};

class LifecycleStrategy final : public Strategy {
 public:
  LifecycleConfig& config() noexcept override { return config_; }

  ErrorCode OnStart() noexcept override {
    if (state_ != State::kIdle || !config_.ready()) {
      return ErrorCode::kInvalidState;
    }
    const auto message = config_.startup_message();
    std::fprintf(stderr, "lifecycle_strategy: startup_message=%.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::fflush(stderr);
    state_ = State::kRunning;
    return ErrorCode::kOk;
  }

  ErrorCode OnStop() noexcept override {
    if (state_ != State::kRunning) {
      return ErrorCode::kInvalidState;
    }
    state_ = State::kStopped;
    return ErrorCode::kOk;
  }

 private:
  enum class State { kIdle, kRunning, kStopped };

  LifecycleConfig config_;
  State state_ = State::kIdle;
};

ErrorCode CreateStrategy(Strategy*& output) noexcept {
  output = nullptr;
  auto* strategy = new (std::nothrow) LifecycleStrategy;
  if (strategy == nullptr) {
    return ErrorCode::kResourceExhausted;
  }
  output = strategy;
  return ErrorCode::kOk;
}

void DestroyStrategy(Strategy* strategy) noexcept { delete strategy; }

const StrategyPlugin kPlugin = {
    yquant::strategy::kStrategyPluginAbiVersion,
    static_cast<std::uint32_t>(sizeof(StrategyPlugin)), CreateStrategy,
    DestroyStrategy};

}  // namespace

extern "C" YQUANT_STRATEGY_EXPORT const StrategyPlugin*
YQuantGetStrategyPlugin() noexcept {
  return &kPlugin;
}
