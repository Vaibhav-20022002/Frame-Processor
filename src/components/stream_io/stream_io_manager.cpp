/**
 * @file stream_io_manager.cpp
 * @brief Implements the StreamIoManager class for handling multiple video input streams.
 *
 * @details This file contains the implementation of the StreamIoManager which:
 * - Manages multiple concurrent RTSP video streams.
 * - Handles dynamic stream addition/removal and configuration changes via a robust
 * reconciliation loop.
 * - Decodes video frames using FFmpeg's libav* libraries.
 * - Applies per-event FPS control through a single, efficient multi-output FFmpeg filter graph.
 * - Provides thread-safe frame delivery to downstream processors.
 */
#include "stream_io_manager.h"

#include <chrono>
#include <set>
#include <sstream>

#include "utils/logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/rational.h>
}

using namespace std::chrono_literals;

namespace {
bool setup_multi_output_filter_graph(UniqueAVFilterGraph &filter_graph,
        AVFilterContext                                 **buffersrc_ctx,
        std::map<std::string, AVFilterContext *>         &sink_contexts,
        AVStream                                         *video_stream,
        const std::vector<EventConfig>                   &events) {

  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs  = avfilter_inout_alloc();
  if (!outputs || !inputs) {
    ERROR_MSG("Failed to allocate filter in/out.");
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  filter_graph.reset(avfilter_graph_alloc());
  if (!filter_graph) {
    ERROR_MSG("Failed to allocate filter graph.");
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  filter_graph->nb_threads = 1;

  const AVFilter    *buffersrc = avfilter_get_by_name("buffer");
  AVCodecParameters *codecpar  = video_stream->codecpar;

  /// Build args but omit pix_fmt if codecpar->format is unknown.
  std::stringstream args_ss;
  args_ss << "width=" << codecpar->width << ":";
  args_ss << "height=" << codecpar->height << ":";
  if (codecpar->format != AV_PIX_FMT_NONE) {
    args_ss << "pix_fmt=" << static_cast<int>(codecpar->format) << ":";
  }
  args_ss << "time_base=" << video_stream->time_base.num << "/" << video_stream->time_base.den
          << ":";
  args_ss << "pixel_aspect=" << codecpar->sample_aspect_ratio.num << "/"
          << codecpar->sample_aspect_ratio.den;
  std::string args = args_ss.str();

  int ret = avfilter_graph_create_filter(
          buffersrc_ctx, buffersrc, "in", args.c_str(), nullptr, filter_graph.get());
  if (ret < 0) {
    char err_buf[256];
    av_strerror(ret, err_buf, sizeof(err_buf));
    ERROR_MSG("Cannot create buffer source filter: {}", err_buf);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  /// Build filter specification: split + fps chains
  std::stringstream filter_spec_ss;
  filter_spec_ss << "[in]split=" << events.size();
  for (size_t i = 0; i < events.size(); ++i) filter_spec_ss << "[s" << i << "]";
  for (size_t i = 0; i < events.size(); ++i) {
    filter_spec_ss << ";[s" << i << "]fps=" << events[i].target_fps << "[sink" << i << "]";
  }
  std::string filter_spec = filter_spec_ss.str();
  INFO_MSG("[Stream {}] Initializing multi-output filter graph with spec: {}",
          video_stream->id,
          filter_spec);

  /// Create sinks and set sink pix_fmts: prefer codecpar->format when valid, else fallback to YUV420P
  AVPixelFormat preferred = (codecpar->format != AV_PIX_FMT_NONE)
          ? static_cast<AVPixelFormat>(codecpar->format)
          : AV_PIX_FMT_YUV420P;

  AVFilterInOut *last_inputs = inputs;
  for (size_t i = 0; i < events.size(); ++i) {
    AVFilterContext *sink_ctx   = nullptr;
    const AVFilter  *buffersink = avfilter_get_by_name("buffersink");
    std::string      sink_name  = "sink" + std::to_string(i);

    ret = avfilter_graph_create_filter(
            &sink_ctx, buffersink, sink_name.c_str(), nullptr, nullptr, filter_graph.get());
    if (ret < 0) {
      char err_buf[256];
      av_strerror(ret, err_buf, sizeof(err_buf));
      ERROR_MSG("Cannot create buffer sink filter: {}", err_buf);
      avfilter_inout_free(&inputs);
      avfilter_inout_free(&outputs);
      return false;
    }

    /// Set allowed pixel formats for the sink (preferred then end marker)
    AVPixelFormat pix_fmts[] = {preferred, AV_PIX_FMT_NONE};
    av_opt_set_int_list(sink_ctx,
            "pix_fmts",
            reinterpret_cast<const int *>(pix_fmts),
            AV_PIX_FMT_NONE,
            AV_OPT_SEARCH_CHILDREN);

    sink_contexts[events[i].name] = sink_ctx;

    last_inputs->name       = av_strdup(("sink" + std::to_string(i)).c_str());
    last_inputs->filter_ctx = sink_ctx;
    last_inputs->pad_idx    = 0;
    last_inputs->next       = (i < events.size() - 1) ? avfilter_inout_alloc() : nullptr;
    last_inputs             = last_inputs->next;
  }

  outputs->name       = av_strdup("in");
  outputs->filter_ctx = *buffersrc_ctx;
  outputs->pad_idx    = 0;
  outputs->next       = nullptr;

  if ((ret = avfilter_graph_parse_ptr(
               filter_graph.get(), filter_spec.c_str(), &inputs, &outputs, nullptr)) < 0) {
    char err_buf[256];
    av_strerror(ret, err_buf, sizeof(err_buf));
    ERROR_MSG("Failed to parse filter graph spec: {}", err_buf);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return false;
  }

  if ((ret = avfilter_graph_config(filter_graph.get(), nullptr)) < 0) {
    char err_buf[256];
    av_strerror(ret, err_buf, sizeof(err_buf));
    ERROR_MSG("Failed to configure filter graph: {}", err_buf);
    return false;
  }

  return true;
}
} // namespace

StreamIoManager::StreamIoManager(std::shared_ptr<fifo<DecodedFrame>> decoded_frame_queue)
    : decoded_frame_queue_(std::move(decoded_frame_queue)) {
  av_log_set_level(AV_LOG_ERROR);
}

StreamIoManager::~StreamIoManager() {
  stop_all();
}

void StreamIoManager::update_streams(const std::vector<StreamConfig> &new_configs) {
  std::lock_guard lock(manager_mutex_);

  std::map<long long, StreamConfig> desired_configs;
  for (const auto &cfg : new_configs) {
    if (cfg.enabled) {
      desired_configs[cfg.id] = cfg;
    }
  }

  std::vector<long long>    to_stop;
  std::vector<StreamConfig> to_start;

  /// Phase 1: Determine what needs to change
  for (const auto &[id, thread] : stream_threads_) {
    auto it = desired_configs.find(id);
    if (it == desired_configs.end() || !(it->second == active_stream_configs_.at(id))) {
      to_stop.push_back(id);
    }
  }

  for (const auto &[id, config] : desired_configs) {
    if (stream_threads_.find(id) == stream_threads_.end()) {
      to_start.push_back(config);
    }
  }

  /// Phase 2: Execute stopping (this is a blocking operation)
  if (!to_stop.empty()) {
    INFO_MSG("Stopping {} stream thread(s)...", to_stop.size());
    for (const auto &id : to_stop) {
      stream_threads_.at(id).request_stop();
      stream_threads_.erase(id);
      active_stream_configs_.erase(id);
    }
    INFO_MSG("Finished stopping threads.");
  }

  /// Phase 3: Execute starting
  if (!to_start.empty()) {
    INFO_MSG("Starting {} new stream thread(s)...", to_start.size());
    for (const auto &config : to_start) {
      active_stream_configs_[config.id] = config;
      stream_threads_[config.id] =
              std::jthread([this, config](std::stop_token st) { this->run_stream(config, st); });
    }
  }
}

void StreamIoManager::stop_all() {
  std::lock_guard lock(manager_mutex_);
  if (stream_threads_.empty()) return;
  INFO_MSG("Stopping all stream I/O threads...");
  stream_threads_.clear();
  active_stream_configs_.clear();
  INFO_MSG("All stream I/O threads stopped.");
}

void StreamIoManager::run_stream(StreamConfig config, std::stop_token st) {
  set_current_thread_name(fmt::format("stream_io_{}", config.id));
  INFO_MSG("[Stream {}] Thread started for {} event(s).", config.id, config.events.size());

  AVFormatContext *format_context = nullptr;
  AVDictionary    *options        = nullptr;
  av_dict_set(&options, "stimeout", "5000000", 0);
  av_dict_set(&options, "rtsp_transport", "tcp", 0);

  if (avformat_open_input(&format_context, config.url.c_str(), nullptr, &options) != 0) {
    ERROR_MSG("[Stream {}] Could not open input URL: {}", config.id, config.url);
    if (options) {
      av_dict_free(&options);
    }
    return;
  }

  UniqueAVFormatContext ctx_guard(format_context);
  if (avformat_find_stream_info(ctx_guard.get(), nullptr) < 0) {
    return;
  }

  int video_stream_index =
          av_find_best_stream(ctx_guard.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_stream_index < 0) {
    return;
  }

  AVStream      *video_stream = ctx_guard->streams[video_stream_index];
  const AVCodec *codec        = avcodec_find_decoder(video_stream->codecpar->codec_id);
  if (!codec) {
    return;
  }

  UniqueAVCodecContext codec_context(avcodec_alloc_context3(codec));
  if (!codec_context ||
          avcodec_parameters_to_context(codec_context.get(), video_stream->codecpar) < 0) {
    return;
  }
  codec_context->thread_count = 1;
  if (avcodec_open2(codec_context.get(), codec, nullptr) < 0) {
    return;
  }

  UniqueAVFilterGraph                      filter_graph;
  AVFilterContext                         *buffersrc_ctx = nullptr;
  std::map<std::string, AVFilterContext *> sink_contexts;

  if (!setup_multi_output_filter_graph(
              filter_graph, &buffersrc_ctx, sink_contexts, video_stream, config.events)) {
    ERROR_MSG("[Stream {}] Failed to initialize multi-output filter graph.", config.id);
    return;
  }

  while (!st.stop_requested()) {
    UniqueAVPacket packet(av_packet_alloc());
    if (av_read_frame(ctx_guard.get(), packet.get()) < 0) {
      WARN_MSG("[Stream {}] av_read_frame failed (EOF or error). Flushing.", config.id);
      break;
    }
    if (packet->stream_index != video_stream_index) continue;

    if (avcodec_send_packet(codec_context.get(), packet.get()) == 0) {
      while (true) {
        UniqueAVFrame raw_frame(av_frame_alloc());
        int           ret = avcodec_receive_frame(codec_context.get(), raw_frame.get());
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        DEBUG_MSG("[Stream {}] Decoded raw frame with size {}x{}",
                config.id,
                raw_frame->width,
                raw_frame->height);

        if (av_buffersrc_add_frame_flags(
                    buffersrc_ctx, raw_frame.get(), AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
          break;

        for (const auto &event : config.events) {
          AVFilterContext *sink_ctx = sink_contexts.at(event.name);
          while (true) {
            UniqueAVFrame filtered_frame(av_frame_alloc());
            int           ret = av_buffersink_get_frame(sink_ctx, filtered_frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            DecodedFrame output_frame;
            output_frame.frame         = std::move(filtered_frame);
            output_frame.source_config = config;
            output_frame.event_config  = event;
            output_frame.time_base     = av_buffersink_get_time_base(sink_ctx);

            decoded_frame_queue_->push(std::move(output_frame));
          }
        }
      }
    }
  }

  INFO_MSG("[Stream {}] Flushing remaining frames...", config.id);
  (void)avcodec_send_packet(codec_context.get(), nullptr);
  while (true) {
    UniqueAVFrame raw_frame(av_frame_alloc());
    if (avcodec_receive_frame(codec_context.get(), raw_frame.get()) < 0) break;
    (void)av_buffersrc_add_frame(buffersrc_ctx, raw_frame.get());
  }
  (void)av_buffersrc_add_frame(buffersrc_ctx, nullptr);
  for (const auto &event : config.events) {
    AVFilterContext *sink_ctx = sink_contexts.at(event.name);
    while (true) {
      UniqueAVFrame filtered_frame(av_frame_alloc());
      if (av_buffersink_get_frame(sink_ctx, filtered_frame.get()) < 0) break;

      DecodedFrame output_frame;
      output_frame.frame         = std::move(filtered_frame);
      output_frame.source_config = config;
      output_frame.event_config  = event;
      output_frame.time_base     = av_buffersink_get_time_base(sink_ctx);
      decoded_frame_queue_->push(std::move(output_frame));
    }
  }

  INFO_MSG("[Stream {}] Thread finished.", config.id);
}