/**
 * @file logger.cpp
 * @brief Implements the global logger variable and helper functions.
 */
#include "logger.h"

#include <string>

// Initialize the global log level. Default to INFO
std::atomic<LogLevel> G_LOG_LEVEL{LogLevel::INFO};

LogLevel string_to_loglevel(const std::string &level_str) {
  if (level_str.empty()) return LogLevel::INFO;

  switch (std::toupper(level_str[0])) {
    case 'F':
      return LogLevel::FAIL;
    case 'E':
      return LogLevel::ERROR;
    case 'W':
      return LogLevel::WARN;
    case 'I':
      return LogLevel::INFO;
    case 'D':
      return LogLevel::DEBUG;
    default:
      return LogLevel::INFO;
  }
}