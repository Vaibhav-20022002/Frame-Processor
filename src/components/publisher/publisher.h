/**
 * @file publisher.h
 * @brief Defines the Publisher class for batching and sending processed frames
 */
#pragma once
#include <atomic>
#include <map>
#include <thread>
#include <vector>

#include "utils/fifo.h"
#include "utils/types.h"

/// Forward declaration for the Redis connection wrapper
class RedisConnection;

/**
 * @struct FrameBatch
 * @brief Represents a batch of processed frames for a single stream, ready for publishing.
 */
struct FrameBatch {
  int64_t     stream_id;
  std::string event_name;
  int         width;
  int         height;
  int         frame_count = 0;
  /// Pre-allocated, contiguous buffer for all frame data in the batch
  std::vector<uint8_t> data_buffer;
};

/**
 * @class Publisher
 * @brief Consumes processed frames, batches them by stream ID, and publishes them to Redis.
 */
class Publisher {
public:
  Publisher(std::shared_ptr<fifo<DecodedFrame>> frame_queue);
  ~Publisher();
  void stop();

private:
  void batching_loop(std::stop_token st);
  void publisher_loop(std::stop_token st, int publisher_id);

  /// Redis Connection Info
  std::string redis_host_;
  int         redis_port_;
  std::string redis_queue_base_name_;

  std::shared_ptr<fifo<DecodedFrame>> processed_frame_queue_;
  fifo<FrameBatch>                    batch_queue_;

  std::jthread              batching_thread_;
  std::vector<std::jthread> publisher_threads_;
  std::atomic<bool>         is_enabled_{false};
};