/**
 * @brief logger.h
 *
 * @brief Defines a runtime-configuration, macro-based logging utility.
 * @details This logger uses a global atomic log level that can be changed at runtime.
 *          Log macros perform a quick check against this level before formatting, making
 *          disabled log calls very cheap.
 *
 * @example
 * INFO_MSG("This is an info message with a number: {}", 123);
 * ERROR_MSG("Something went wrong with error code: {}", err_code);
 */
#pragma once

#include <atomic>
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/core.h>
#include <string>
#include <string_view>

/**
 * @enum LogLevel
 * @brief Defines the different levels of logging verbosity.
 */
enum class LogLevel { FAIL = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

// GLOBAL thread-safe log-level for the whole app
extern std::atomic<LogLevel> G_LOG_LEVEL;

/**
 * @brief Converts a string representation of a loglevel to the LogLevel enum.
 * @param level_str The string to convert
 * @return The corresponding LogLevel enum. Defaults to LogLevel::INFO.
 */
LogLevel string_to_loglevel(const std::string &level_str);

/**
 * @internal
 * @brief The core logging print function. Not intended for direct use.
 *
 * @note Accept any fmt-format type (compile-time or runtime) as the format parameter.
 */
template<typename Format, typename... Args>
void _log_print(FILE    *stream,
        std::string_view level_str,
        std::string_view file,
        int              line,
        const Format    &format,
        Args &&...args) {
  fmt::print(stream,
          "[{:%Y-%m-%d %H:%M:%S}] [{}] [{}:{}] ",
          fmt::localtime(std::time(nullptr)),
          level_str,
          file,
          line);
  // fmt::print supports compile-time format objects as well as runtime strings
  fmt::print(stream, format, std::forward<Args>(args)...);
  fmt::print(stream, "\n");
  fflush(stream);
}

// Use .load() on the atomic, and accept standard __VA_ARGS__ style.
#define FAIL_MSG(fmt, ...)                                                             \
  do {                                                                                 \
    if (G_LOG_LEVEL.load() >= LogLevel::FAIL)                                          \
      _log_print(stderr, "FAIL ", __FILE__, __LINE__, FMT_STRING(fmt), ##__VA_ARGS__); \
  } while (0)
#define ERROR_MSG(fmt, ...)                                                            \
  do {                                                                                 \
    if (G_LOG_LEVEL.load() >= LogLevel::ERROR)                                         \
      _log_print(stderr, "ERROR", __FILE__, __LINE__, FMT_STRING(fmt), ##__VA_ARGS__); \
  } while (0)
#define WARN_MSG(fmt, ...)                                                             \
  do {                                                                                 \
    if (G_LOG_LEVEL.load() >= LogLevel::WARN)                                          \
      _log_print(stdout, "WARN ", __FILE__, __LINE__, FMT_STRING(fmt), ##__VA_ARGS__); \
  } while (0)
#define INFO_MSG(fmt, ...)                                                             \
  do {                                                                                 \
    if (G_LOG_LEVEL.load() >= LogLevel::INFO)                                          \
      _log_print(stdout, "INFO ", __FILE__, __LINE__, FMT_STRING(fmt), ##__VA_ARGS__); \
  } while (0)
#define DEBUG_MSG(fmt, ...)                                                            \
  do {                                                                                 \
    if (G_LOG_LEVEL.load() >= LogLevel::DEBUG)                                         \
      _log_print(stdout, "DEBUG", __FILE__, __LINE__, FMT_STRING(fmt), ##__VA_ARGS__); \
  } while (0)
