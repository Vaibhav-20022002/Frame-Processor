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
 * @brief A thread-safe RAII wrapper for a hiredis connection.
 * @details Ensures redisFree is called automatically when the object goes out of scope.
 */
class RedisConnection {
public:
  /**
   * @brief Constructs and establishes a connection to the Redis server.
   * @param host The hostname or IP of the Redis server.
   * @param port The port number of the Redis server.
   */
  RedisConnection(const std::string &host, int port) {
    context_ = redisConnect(host.c_str(), port);
    if (context_ == nullptr || context_->err) {
      if (context_) {
        ERROR_MSG("Redis connection error: {}", context_->errstr);
        redisFree(context_);
        context_ = nullptr;
      } else {
        FAIL_MSG("Could not allocate redis context.");
      }
    } else {
      INFO_MSG("Successfully connected to Redis at {}:{}.", host, port);
    }
  }

  /**
   * @brief Destructor. Frees the Redis context if it's valid.
   */
  ~RedisConnection() {
    if (context_) {
      redisFree(context_);
    }
  }

  /// Disable copy semantics
  RedisConnection(const RedisConnection &)            = delete;
  RedisConnection &operator=(const RedisConnection &) = delete;

  /// Enable move semantics
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
   * @brief Checks if the connection is valid.
   * @return True if connected, false otherwise.
   */
  bool is_valid() const { return context_ != nullptr; }

  /**
   * @brief Provides access to the raw hiredis context pointer.
   * @return The raw `redisContext*` handle.
   */
  redisContext *get() const { return context_; }

private:
  redisContext *context_ = nullptr;
};