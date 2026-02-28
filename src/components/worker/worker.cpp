/**
 * @file worker.cpp
 * @brief Implements the Worker class for parallel frame processing.
 *
 * @details This file contains the implementation of the Worker class which manages
 * a pool of worker threads for processing decoded video frames. Each worker thread:
 * - Retrieves frames from a shared queue
 * - Applies configured transformations (crop/scale) using FFmpeg filters
 * - Handles filter graph initialization and reuse
 * - Ensures thread-safe processing of multiple video streams
 *
 * The worker threads process frames using FFmpeg's filter API, with support for:
 * - Dynamic filter graph creation based on processing configuration
 * - Frame cropping and scaling operations
 * - Error recovery and logging
 */
#include "worker.h"

#include <fmt/core.h>
#include <sstream>
#include <stop_token>
#include <unordered_map>

#include "utils/logger.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
}

namespace {
/**
 * @brief Creates a new processing (crop and/or scale) filter graph for a frame.
 *
 * @details This function initializes an FFmpeg filter graph with the following capabilities:
 * - Frame cropping: Extracts a region of interest from the frame
 * - Frame scaling: Resizes the frame to target dimensions
 * - Pixel format conversion: Ensures consistent output format
 *
 * The filter graph is created in a specific order:
 * 1. Allocates the filter graph container
 * 2. Creates buffer source filter (input)
 * 3. Creates buffer sink filter (output)
 * 4. Sets up filter chain based on processing configuration
 * 5. Validates and configures the complete graph
 *
 * @param context The processing context to store the created filter graph
 * @param input_frame The source frame used to determine input parameters
 * @param proc_cfg Processing configuration specifying crop and scale parameters
 * @param time_base The time base of the input stream for frame timing
 *
 * @return `true` on successful filter graph creation, `false` on any failure
 *
 * @note The created filter graph is stored in the context and must be properly
 *       freed when no longer needed.
 */
bool create_processing_filter(StreamProcessingContext &context,
        const AVFrame                                 *input_frame,
        const ProcessingConfig                        &proc_cfg,
        const AVRational                              &time_base) {
  context.filter_graph.reset(avfilter_graph_alloc());
  if (!context.filter_graph) return false;

  const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
  const AVFilter *buffersink = avfilter_get_by_name("buffersink");
  AVFilterInOut  *outputs    = avfilter_inout_alloc();
  AVFilterInOut  *inputs     = avfilter_inout_alloc();

  if (!outputs || !inputs) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  std::string args = fmt::format("width={}:height={}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}",
          input_frame->width,
          input_frame->height,
          static_cast<int>(input_frame->format),
          1,
          1000,
          input_frame->sample_aspect_ratio.num,
          input_frame->sample_aspect_ratio.den);

  int ret = avfilter_graph_create_filter(&context.buffersrc_ctx,
          buffersrc,
          "in",
          args.c_str(),
          nullptr,
          context.filter_graph.get());
  if (ret < 0) {
    ERROR_MSG("Cannot create buffer source filter.");
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  ret = avfilter_graph_create_filter(
          &context.buffersink_ctx, buffersink, "out", nullptr, nullptr, context.filter_graph.get());
  if (ret < 0) {
    ERROR_MSG("Cannot create buffer sink filter.");
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  AVPixelFormat pix_fmts[] = {(AVPixelFormat)input_frame->format, AV_PIX_FMT_NONE};
  av_opt_set_int_list(
          context.buffersink_ctx, "pix_fmts", pix_fmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);

  outputs->name       = av_strdup("in");
  outputs->filter_ctx = context.buffersrc_ctx;
  outputs->pad_idx    = 0;
  outputs->next       = nullptr;
  inputs->name        = av_strdup("out");
  inputs->filter_ctx  = context.buffersink_ctx;
  inputs->pad_idx     = 0;
  inputs->next        = nullptr;

  // **--- Build the filter spec string ---**
  std::stringstream filter_spec_ss;
  if (proc_cfg.crop.width > 0 && proc_cfg.crop.height > 0) {
    filter_spec_ss << "crop=" << proc_cfg.crop.width << ":" << proc_cfg.crop.height << ":"
                   << proc_cfg.crop.x << ":" << proc_cfg.crop.y;
  }
  if (proc_cfg.scale.width > 0 && proc_cfg.scale.height > 0) {
    if (filter_spec_ss.tellp() > 0) filter_spec_ss << ",";
    filter_spec_ss << "scale=" << proc_cfg.scale.width << ":" << proc_cfg.scale.height;
  }

  std::string filter_spec = filter_spec_ss.str();
  if (filter_spec.empty()) { // No processing needed, create a passthrough graph
    filter_spec = "null";
  }

  INFO_MSG("Initializing worker filter graph with spec: [{}]", filter_spec);

  if (avfilter_graph_parse_ptr(
              context.filter_graph.get(), filter_spec.c_str(), &inputs, &outputs, nullptr) < 0) {
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }
  if (avfilter_graph_config(context.filter_graph.get(), nullptr) < 0) return false;
  return true;
}
} // namespace

/**
 * @brief Constructs a new Worker instance.
 *
 * @details Initializes a worker pool that will process decoded frames using
 * multiple threads. The worker is designed to:
 * - Process frames from multiple video streams concurrently
 * - Apply configured transformations (crop/scale) to each frame
 * - Handle dynamic stream configuration changes
 * - Enqueues processed frames to a queue
 *
 * @param in_queue Shared queue from which worker threads retrieve frames
 * @param out_queue Shared queue to which worker threads enqueue processed frames
 * @param num_threads Number of worker threads to create in the pool
 *
 * @note The in_queue and out_queue are shared between this worker and the stream IO manager,
 *       and Publisher which produces/consumes the frames.
 *       The queue implementation guarantees thread safety.
 */
Worker::Worker(std::shared_ptr<fifo<DecodedFrame>> in_queue,
        std::shared_ptr<fifo<DecodedFrame>>        out_queue,
        int                                        num_threads)
    : in_queue_(std::move(in_queue))
    , out_queue_(std::move(out_queue))
    , frame_saver_(std::make_shared<FrameSaver>())
    , num_threads_(num_threads) {

  std::string env_level_str = "AV_LOG_ERROR";
  if (const char *val = std::getenv("FFMPEG_LOG_LEVEL")) env_level_str = val;

  static const std::unordered_map<std::string, int> log_level_map = {{"AV_LOG_QUIET", AV_LOG_QUIET},
          {"AV_LOG_PANIC", AV_LOG_PANIC},
          {"AV_LOG_FATAL", AV_LOG_FATAL},
          {"AV_LOG_ERROR", AV_LOG_ERROR},
          {"AV_LOG_WARNING", AV_LOG_WARNING},
          {"AV_LOG_INFO", AV_LOG_INFO},
          {"AV_LOG_VERBOSE", AV_LOG_VERBOSE},
          {"AV_LOG_DEBUG", AV_LOG_DEBUG},
          {"AV_LOG_TRACE", AV_LOG_TRACE}};

  int final_level = AV_LOG_ERROR;
  if (auto it = log_level_map.find(env_level_str); it != log_level_map.end()) {
    final_level = it->second;
  }
  av_log_set_level(final_level);
}

Worker::~Worker() {
  stop();
}

/**
 * @brief Starts the worker pool's processing threads.
 *
 * @details This method initializes and launches the worker thread pool:
 * 1. Creates the specified number of worker threads
 * 2. Each thread runs the worker_loop() method
 * 3. Threads begin processing frames from the shared queue
 *
 * The number of threads is determined at construction time and is typically
 * based on the available CPU cores minus 2.
 *
 * @note Once started, threads will continue processing until stop() is called
 *       or the frame queue is stopped externally.
 */
void Worker::start() {
  INFO_MSG("Starting worker manager with {} threads.", num_threads_);
  for (int i = 0; i < num_threads_; ++i) {
    workers_.emplace_back([this, i](std::stop_token st) { this->worker_loop(st, i); });
  }
}

/**
 * @brief Stops all worker threads gracefully.
 *
 * @details This method performs an orderly shutdown of the worker pool:
 * 1. Signals the frame queue to stop accepting new frames
 * 2. Allows threads to finish processing current frames
 * 3. Cleans up thread resources
 *
 * The shutdown sequence ensures that:
 * - No frames are lost (all queued frames are processed)
 * - Resources are properly released
 * - No threads are terminated abruptly
 *
 * @note This method is synchronous and will block until all threads have stopped.
 */
void Worker::stop() {
  INFO_MSG("Stopping worker manager...");
  in_queue_->stop();
  out_queue_->stop();
  workers_.clear();
  INFO_MSG("All worker threads stopped.");
}

/**
 * @brief Main processing loop for a worker thread.
 *
 * @details This method implements the core frame processing logic:
 * 1. Maintains a cache of filter graphs per stream to avoid recreating them
 * 2. Processes frames from the shared central queue until stopped
 * 3. Handles frame transformations using FFmpeg filter graphs
 * 4. Manages processing context lifecycle and updates
 *
 * The processing sequence for each frame:
 * - Extract frame metadata and configuration
 * - Check/update filter graph for the stream
 * - Apply transformations (crop/scale)
 * - Validate output dimensions
 * - Log processing results
 *
 * @param stop_token Thread control token for cooperative shutdown
 * @param worker_id Unique id for the worker thread
 *
 * @note The worker maintains separate processing contexts for each stream to
 *       handle multiple video sources efficiently. Filter graphs are reused
 *       when possible and recreated only when stream parameters change.
 */
void Worker::worker_loop(std::stop_token st, int worker_id) {
  /// Set the thread name using the passed-in ID
  set_current_thread_name(fmt::format("worker_{}", worker_id));

  INFO_MSG("Worker thread started.");

  /// Map of string (stream_id + event_name) to their processing contexts
  /// This cache allows reuse of filter graphs when possible
  std::unordered_map<std::string, StreamProcessingContext> stream_contexts;

  while (!st.stop_requested()) {
    auto frame_optional = in_queue_->wait_and_pop();
    if (!frame_optional) {
      break;
    }

    DecodedFrame      d_frame     = std::move(*frame_optional);
    const AVFrame    *input_frame = d_frame.frame.get();
    const auto       &config      = d_frame.source_config;
    const auto       &proc_cfg    = config.processing;
    const std::string ctx_key     = fmt::format("{}_{}", config.id, d_frame.event_config.name);

    auto start_time = std::chrono::high_resolution_clock::now();

    // **---- Step 1 - Save the initial metadata before any processing. ----**

    /// We store the original properties in local variables so we can compare
    /// them against the final output later, ensuring our original frame data
    /// is never lost or corrupted.
    const int     original_width  = input_frame->width;
    const int     original_height = input_frame->height;
    const int64_t original_pts    = input_frame->pts;

    // **---- Filter Graph Context Management ----**

    /// Check if we need to reinitialize the filter graph for this stream.
    bool needs_reinit = true;
    if (auto it = stream_contexts.find(ctx_key); it != stream_contexts.end()) {
      auto &ctx = it->second;
      if (ctx.active_config == proc_cfg &&                     //< Same processing settings
              ctx.last_input_width == original_width &&        //< Same frame width
              ctx.last_input_height == original_height &&      //< Same frame height
              ctx.last_input_pix_fmt == input_frame->format) { //< Same pixel format
        needs_reinit = false;
      }
    }
    if (needs_reinit) {
      INFO_MSG("Worker re-initializing context for Stream ID: {}, Event: {}",
              config.id,
              d_frame.event_config.name);
      StreamProcessingContext new_ctx;
      new_ctx.active_config      = proc_cfg;
      new_ctx.last_input_width   = original_width;
      new_ctx.last_input_height  = original_height;
      new_ctx.last_input_pix_fmt = (AVPixelFormat)input_frame->format;
      if (!create_processing_filter(new_ctx, input_frame, proc_cfg, d_frame.time_base)) {
        ERROR_MSG("Failed to create processing filter for Stream ID: {}", config.id);
        continue;
      }
      stream_contexts[ctx_key] = std::move(new_ctx);
    }

    auto &context = stream_contexts.at(ctx_key);

    // **---- Frame Processing Pipeline ----**

    /// Step 1: Clone the input frame to avoid modifying the original
    UniqueAVFrame frame_to_process(av_frame_clone(input_frame));
    if (!frame_to_process) {
      WARN_MSG("Could not clone frame for processing for Stream ID: {}", config.id);
      continue;
    }

    /// Step 2: Feed the frame into the filter graph for processing
    if (av_buffersrc_add_frame(context.buffersrc_ctx, frame_to_process.get()) < 0) {
      WARN_MSG("Could not feed frame to worker filter for Stream ID: {}", config.id);
      continue;
    }

    /// Step 3: Retrieve and validate processed frames
    int frames_drained = 0;
    while (frames_drained < 5) {
      UniqueAVFrame proc_frame(av_frame_alloc());
      int           flags = (frames_drained == 0) ? 0 : AV_BUFFERSINK_FLAG_NO_REQUEST;
      int ret = av_buffersink_get_frame_flags(context.buffersink_ctx, proc_frame.get(), flags);

      /// EAGAIN means no frames available yet
      /// EOF means filter graph is done
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        break;
      }

      /// Check for other errors during frame retrieval
      if (ret < 0) {
        WARN_MSG("Could not get frame from worker filter for Stream ID: {}", config.id);
        break;
      }

      ++frames_drained;

      // **---- NEW: Step 2 - Verify the processed frame and log everything ----**

      int expected_width  = original_width;
      int expected_height = original_height;

      /// The final expected dimensions are the scale dimensions if they are set,
      /// otherwise they are the crop dimensions.
      if (proc_cfg.scale.width > 0 && proc_cfg.scale.height > 0) {
        expected_width  = proc_cfg.scale.width;
        expected_height = proc_cfg.scale.height;
      } else if (proc_cfg.crop.width > 0 && proc_cfg.crop.height > 0) {
        expected_width  = proc_cfg.crop.width;
        expected_height = proc_cfg.crop.height;
      }

      bool dims_ok = (proc_frame->width == expected_width && proc_frame->height == expected_height);

      DEBUG_MSG("Frame Processed [Stream {}]: Original({}x{}, PTS:{}) -> Final({}x{}, PTS:{}) | "
                "Target(Crop:{}x{}, Scale:{}x{}) | Verified: {}",
              config.id,
              original_width,
              original_height,
              original_pts,
              proc_frame->width,
              proc_frame->height,
              proc_frame->pts,
              proc_cfg.crop.width,
              proc_cfg.crop.height,
              proc_cfg.scale.width,
              proc_cfg.scale.height,
              (dims_ok ? "OK" : "DIMENSION MISMATCH"));

      /// Create a new DecodedFrame for each output frame to prevent moving from an invalid object
      DecodedFrame output_frame{.frame = std::move(proc_frame),
              .source_config           = d_frame.source_config,
              .event_config            = d_frame.event_config,
              .time_base               = d_frame.time_base};

      if (frame_saver_ && frame_saver_->is_processed_enabled()) {
        static thread_local uint64_t frames_processed_local = 0;
        frames_processed_local++;
        frame_saver_->save_frame(output_frame.frame.get(),
                d_frame.source_config.id,
                frames_processed_local,
                "processed");
      }

      out_queue_->push(std::move(output_frame));
    }

    auto                  end_time = std::chrono::high_resolution_clock::now();
    [[maybe_unused]] auto duration_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    DEBUG_MSG("[Worker {}] Critical section duration: {} ms", worker_id, duration_ms);
  }
  INFO_MSG("Worker thread finished.");
}