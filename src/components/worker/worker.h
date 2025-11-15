/**
 * @file worker.h
 * @brief Defines the Worker class for processing decoded frames.
 */
#pragma once
#include <thread>
#include <unordered_map>
#include <vector>

#include "utils/fifo.h"
#include "utils/types.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

/// Forward declarations
struct AVFilterContext;

/**
 * @struct StreamProcessingContext
 * @brief Holds the reusable FFmpeg filter graph for a single video stream.
 */
struct StreamProcessingContext {
  UniqueAVFilterGraph filter_graph;
  AVFilterContext    *buffersrc_ctx  = nullptr;
  AVFilterContext    *buffersink_ctx = nullptr;

  /// Store the parameters used to create the context to detect changes
  ProcessingConfig active_config;
  int              last_input_width   = 0;
  int              last_input_height  = 0;
  AVPixelFormat    last_input_pix_fmt = AV_PIX_FMT_NONE;
};

/**
 * @class Worker
 * @brief Manages a pool of worker threads to process decoded frames.
 */
class Worker {
public:
  Worker(std::shared_ptr<fifo<DecodedFrame>>  in_queue,
          std::shared_ptr<fifo<DecodedFrame>> out_queue,
          int                                 num_threads);
  ~Worker();
  void start();
  void stop();

private:
  void                                worker_loop(std::stop_token stop_token, int worker_id);
  std::vector<std::jthread>           workers_;
  std::shared_ptr<fifo<DecodedFrame>> in_queue_;
  std::shared_ptr<fifo<DecodedFrame>> out_queue_;
  int                                 num_threads_;
};