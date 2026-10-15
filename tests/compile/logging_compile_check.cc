#include <string>
#include <string_view>

#include "yquant/base/logging.h"

#ifndef YQUANT_LOG_COMPILE_CASE
#error "YQUANT_LOG_COMPILE_CASE must select a compile-check case"
#endif

#if YQUANT_LOG_COMPILE_CASE == 5
struct LoggingBusinessObject {};

template <>
struct fmt::formatter<LoggingBusinessObject>
    : fmt::formatter<fmt::string_view> {
  template <typename FormatContext>
  auto format(const LoggingBusinessObject&, FormatContext& context) const {
    return fmt::formatter<fmt::string_view>::format("business", context);
  }
};
#endif

namespace yquant::base {

void CompileLoggingCall(Logger& logger) {
#if YQUANT_LOG_COMPILE_CASE == 0
  // A disabled call must disappear before semantic analysis, even at -O0.
  YQ_LOG_TRACE(logger_that_does_not_exist,
               "YQ_DISABLED_TRACE_CALL_SITE_ONLY_3b43 {}",
               ArgumentThatDoesNotExist());
  YQ_LOG_DEBUG(logger_that_does_not_exist,
               "YQ_DISABLED_DEBUG_CALL_SITE_ONLY_3b43 {}",
               ArgumentThatDoesNotExist());
  YQ_LOG_INFO(logger, "retained-info={}", 17);
#elif YQUANT_LOG_COMPILE_CASE == 1
  YQ_LOG_INFO(logger, "{} {}", 17);
#elif YQUANT_LOG_COMPILE_CASE == 2
  YQ_LOG_INFO(logger, "{:04}", 17);
#elif YQUANT_LOG_COMPILE_CASE == 3
  std::string_view runtime_format = "{}";
  YQ_LOG_INFO(logger, runtime_format, 17);
#elif YQUANT_LOG_COMPILE_CASE == 4
  const char* unbounded_text = "unbounded";
  YQ_LOG_INFO(logger, "{}", unbounded_text);
#elif YQUANT_LOG_COMPILE_CASE == 5
  YQ_LOG_INFO(logger, "{}", LoggingBusinessObject{});
#elif YQUANT_LOG_COMPILE_CASE == 6
  std::string heap_text = "owned string";
  YQ_LOG_INFO(logger, "{}", heap_text);
#elif YQUANT_LOG_COMPILE_CASE == 7
  YQ_LOG_INFO(logger, "{{}} {} {} {} {} {} {} {} {} {} {} {} {} {}", true, 'x',
              static_cast<signed char>(-1), static_cast<unsigned char>(1),
              static_cast<short>(-2), static_cast<unsigned short>(2), -3, 3U,
              -4L, 4UL, -5LL, 5ULL, 1.25F);
  YQ_LOG_INFO(logger, "{} {} {}", 2.5, std::string_view("view"), "literal");
#else
#error "Unknown logging compile-check case"
#endif
}

}  // namespace yquant::base
