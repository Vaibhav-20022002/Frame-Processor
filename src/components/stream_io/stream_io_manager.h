/**
 * @file stream_io_manager.h
 */

#pragma once

#include <map>
#include <mutex>
#include <thread>

#include "utils/fifo.h"
#include "utils/types.h"

class StreamIoManager {
public:
  /**
   * @brief Constructs a new StreamIoManager instance
   *
   * @details Initializes the stream manager with:
   * - A shared frame queue for passing decoded frames downstream
   * - FFmpeg logging configuration
   * - Empty initial stream configuration
   *
   * The manager is designed to handle:
   * - Dynamic stream configuration updates
   * - Thread-safe frame delivery
   * - Graceful stream lifecycle management
   *
   * @param decoded_frame_queue Shared queue for passing decoded frames to processors
   */
  explicit StreamIoManager(std::shared_ptr<fifo<DecodedFrame>> decoded_frame_queue);

  /**
   * @brief Destroys the StreamIoManager, ensuring clean shutdown
   *
   * Calls stop_all() to ensure:
   * - All stream threads are terminated gracefully
   * - Resources are properly freed
   * - No memory leaks from FFmpeg contexts
   */
  ~StreamIoManager();

  /**
   * @brief Updates the set of active streams based on new configuration
   *
   * @details This method handles the dynamic reconfiguration of streams by:
   * 1. Identifying streams to add, remove, or update
   * 2. Gracefully stopping removed/changed streams
   * 3. Starting new streams with fresh configurations
   * 4. Managing thread lifecycle for each stream
   *
   * The update process ensures:
   * - No interruption of active streams that haven't changed
   * - Clean shutdown of removed/changed streams
   * - Proper initialization of new streams
   * - Thread-safe state transitions
   *
   * @param new_configs Vector of stream configurations to apply
   *
   * @note This method can be called at any time to modify the active streams.
   *       It handles all necessary synchronization internally.
   */
  void update_streams(const std::vector<StreamConfig> &new_configs);
  void stop_all();

private:
  /**
   * @brief Main processing loop for a single video stream
   *
   * @details This method implements the core streaming functionality:
   * 1. Stream Connection and Setup:
   *    - Opens RTSP connection with configurable timeout
   *    - Initializes FFmpeg demuxer and decoder
   *    - Sets up FPS control filter graph
   *
   * 2. Frame Processing Pipeline:
   *    - Reads network packets
   *    - Decodes video frames
   *    - Controls frame rate
   *    - Pushes frames to shared queue
   *
   * 3. Error Handling and Recovery:
   *    - Handles network timeouts
   *    - Manages decoder errors
   *    - Ensures proper resource cleanup
   *
   * @param config Configuration for this stream (URL, FPS, etc.)
   * @param st Stop token for cooperative thread shutdown
   *
   * @note This method runs in its own thread and manages its own lifecycle.
   *       It will continue processing until either:
   *       - The stop token is triggered
   *       - A fatal error occurs
   *       - The stream ends (for non-live streams)
   */
  void run_stream(StreamConfig config, std::stop_token st);

  std::map<int, std::jthread> stream_threads_;
  std::map<int, StreamConfig> active_stream_configs_;

  std::shared_ptr<fifo<DecodedFrame>> decoded_frame_queue_;
  mutable std::mutex                  manager_mutex_;
};