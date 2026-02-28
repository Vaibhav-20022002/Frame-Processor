/**
 * @file redis_client.h
 * @brief A C++ RAII wrapper for a hiredis redisContext.
 */
#pragma once

#include <hiredis/hiredis.h>
#include <string>

#include "utils/logger.h"

/**
 * @class RedisConnection
 * @brief A resource-safe wrapper for a hiredis connection.
 * @attention This class implements the RAII idiom for `redisContext`.
 *
 * - `Constructor`: Connects to Redis.
 *
 * - `Destructor`: Calls `redisFree`.
 *
 * - `Move-Only`: Cannot be copied (connections are unique resources), but can be moved.
 *
 * @warning This class is `NOT` thread-safe. Do not share a single instance across threads
 *          without external synchronization. In `Publisher`, this is handled by `connection_pool_`
 *          and `pool_mutex_`.
 */
class RedisConnection {
public:
  /**
   * @brief Establishes a new TCP connection to Redis.
   * @param host Redis server hostname or IP.
   * @param port Redis server port.
   */
  RedisConnection(const std::string &host, int port) {
    struct timeval timeout = {2, 0}; //< 2 seconds timeout
    context_               = redisConnectWithTimeout(host.c_str(), port, timeout);

    if (context_ == nullptr || context_->err) {
      if (context_) {
        ERROR_MSG("Redis connection error: {}", context_->errstr);
        redisFree(context_);
        context_ = nullptr;
      } else {
        ERROR_MSG("Fatal: Could not allocate redis context.");
      }
    } else {
      /// Set the same timeout for operations (commands)
      if (redisSetTimeout(context_, timeout) != REDIS_OK) {
        WARN_MSG("Failed to set Redis operation timeout for {}:{}.", host, port);
      }
      INFO_MSG("Successfully connected to Redis at {}:{}.", host, port);
    }
  }

  /**
   * @brief Destructor. Automatically closes the connection.
   */
  ~RedisConnection() {
    if (context_) {
      redisFree(context_);
    }
  }

  /// Disable copy semantics (a connection cannot be duplicated easily)
  RedisConnection(const RedisConnection &)            = delete;
  RedisConnection &operator=(const RedisConnection &) = delete;

  /// Enable move semantics (transfer ownership of the connection)
  RedisConnection(RedisConnection &&other) noexcept
      : context_(other.context_) {
    other.context_ = nullptr;
  }
  RedisConnection &operator=(RedisConnection &&other) noexcept {
    if (this != &other) {
      if (context_) redisFree(context_);
      context_       = other.context_;
      other.context_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief Checks if the underlying context is allocated.
   * @note This does not strictly guarantee the socket is still open (keepalives/timeouts),
   *       but it ensures the pointer is valid for use.
   */
  bool is_valid() const { return context_ != nullptr; }

  /**
   * @brief Returns the raw hiredis context for executing commands.
   */
  redisContext *get() const { return context_; }

private:
  redisContext *context_ = nullptr;
};