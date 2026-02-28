/**
 * @file frame_saver.cpp
 * @brief Implementation of the FrameSaver utility class.
 *
 * @details Implements frame saving to JPEG with FFmpeg encoding. Handles:
 *  - YUV420P to RGB conversion via swscale
 *  - JPEG encoding via FFmpeg's mjpeg encoder
 *  - Thread-safe directory creation
 *  - Interval-based frame skipping
 */

#include "frame_saver.h"

#include <cstdlib>
#include <filesystem>
#include <fmt/core.h>

#include "logger.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace {

/**
 * @brief Retrieves an environment variable with a fallback default value.
 */
std::string get_env(const char *var, const std::string &default_value) {
  const char *val = std::getenv(var);
  return val ? val : default_value;
}

/**
 * @brief Parses a boolean-like environment variable.
 * @return true if value is "true", "1", "yes", "on" (case-insensitive).
 */
bool parse_bool_env(const char *var, bool default_value) {
  const char *val = std::getenv(var);
  if (!val) return default_value;

  std::string s(val);
  for (auto &c : s) c = static_cast<char>(std::tolower(c));

  return (s == "true" || s == "1" || s == "yes" || s == "on");
}

} // namespace

FrameSaver::FrameSaver()
    : base_output_dir_(get_env("FP_SAVED_FRAMES_OUTPUT_DIR", "/tmp/output"))
    , save_interval_(std::stoi(get_env("FP_SAVE_FRAME_INTERVAL", "1")))
    , save_decoded_enabled_(parse_bool_env("FP_SAVE_DECODED_FRAMES", false))
    , save_processed_enabled_(parse_bool_env("FP_SAVE_PROCESSED_FRAMES", false)) {

  /// Ensure interval is at least 1
  if (save_interval_ < 1) {
    WARN_MSG("FrameSaver: Invalid FP_SAVE_FRAME_INTERVAL={}, defaulting to 1", save_interval_);
    save_interval_ = 1;
  }

  if (save_decoded_enabled_ || save_processed_enabled_) {
    INFO_MSG("FrameSaver: Initialized | decoded={} | processed={} | interval={} | dir={}",
            save_decoded_enabled_,
            save_processed_enabled_,
            save_interval_,
            base_output_dir_);
  } else {
    DEBUG_MSG("FrameSaver: Frame saving is disabled (both decoded and processed are off)");
  }
}

bool FrameSaver::ensure_directory_exists(const std::string &path) {
  /// Fast path: check if already created (without lock)
  {
    std::lock_guard lock(dir_creation_mutex_);
    if (created_dirs_.count(path)) {
      return true;
    }
  }

  /// Slow path: create directory
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    if (!fs::create_directories(path, ec)) {
      ERROR_MSG("FrameSaver: Failed to create directory '{}': {}", path, ec.message());
      return false;
    }
    DEBUG_MSG("FrameSaver: Created directory '{}'", path);
  }

  /// Mark as created
  {
    std::lock_guard lock(dir_creation_mutex_);
    created_dirs_.insert(path);
  }

  return true;
}

bool FrameSaver::encode_and_save_jpeg(const AVFrame *frame, const std::string &filepath) {
  /// Find MJPEG encoder
  const AVCodec *jpeg_codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
  if (!jpeg_codec) {
    ERROR_MSG("FrameSaver: MJPEG encoder not found in FFmpeg build");
    return false;
  }

  /// Allocate encoder context
  AVCodecContext *codec_ctx = avcodec_alloc_context3(jpeg_codec);
  if (!codec_ctx) {
    ERROR_MSG("FrameSaver: Failed to allocate JPEG codec context");
    return false;
  }

  /// Configure encoder
  codec_ctx->width     = frame->width;
  codec_ctx->height    = frame->height;
  codec_ctx->time_base = {1, 25};
  codec_ctx->pix_fmt   = AV_PIX_FMT_YUVJ420P; ///< JPEG uses YUVJ420P (full range)

  /// Open encoder
  if (avcodec_open2(codec_ctx, jpeg_codec, nullptr) < 0) {
    ERROR_MSG("FrameSaver: Failed to open JPEG encoder");
    avcodec_free_context(&codec_ctx);
    return false;
  }

  /// Convert frame to YUVJ420P if needed
  AVFrame       *convert_frame = nullptr;
  const AVFrame *src_frame     = frame;

  if (frame->format != AV_PIX_FMT_YUVJ420P) {
    convert_frame = av_frame_alloc();
    if (!convert_frame) {
      ERROR_MSG("FrameSaver: Failed to allocate conversion frame");
      avcodec_free_context(&codec_ctx);
      return false;
    }

    convert_frame->width  = frame->width;
    convert_frame->height = frame->height;
    convert_frame->format = AV_PIX_FMT_YUVJ420P;

    if (av_frame_get_buffer(convert_frame, 32) < 0) {
      ERROR_MSG("FrameSaver: Failed to allocate conversion frame buffer");
      av_frame_free(&convert_frame);
      avcodec_free_context(&codec_ctx);
      return false;
    }

    /// Create swscale context for conversion
    SwsContext *sws_ctx = sws_getContext(frame->width,
            frame->height,
            static_cast<AVPixelFormat>(frame->format),
            frame->width,
            frame->height,
            AV_PIX_FMT_YUVJ420P,
            SWS_BILINEAR,
            nullptr,
            nullptr,
            nullptr);

    if (!sws_ctx) {
      ERROR_MSG("FrameSaver: Failed to create swscale context");
      av_frame_free(&convert_frame);
      avcodec_free_context(&codec_ctx);
      return false;
    }

    /// Perform conversion
    sws_scale(sws_ctx,
            frame->data,
            frame->linesize,
            0,
            frame->height,
            convert_frame->data,
            convert_frame->linesize);

    sws_freeContext(sws_ctx);
    src_frame = convert_frame;
  }

  /// Allocate packet for encoded data
  AVPacket *pkt = av_packet_alloc();
  if (!pkt) {
    ERROR_MSG("FrameSaver: Failed to allocate packet");
    if (convert_frame) av_frame_free(&convert_frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  /// Send frame to encoder
  int ret = avcodec_send_frame(codec_ctx, src_frame);
  if (ret < 0) {
    ERROR_MSG("FrameSaver: Failed to send frame to encoder: {}", ret);
    av_packet_free(&pkt);
    if (convert_frame) av_frame_free(&convert_frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  /// Receive encoded packet
  ret = avcodec_receive_packet(codec_ctx, pkt);
  if (ret < 0) {
    ERROR_MSG("FrameSaver: Failed to receive packet from encoder: {}", ret);
    av_packet_free(&pkt);
    if (convert_frame) av_frame_free(&convert_frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  /// Write to file
  FILE *file = fopen(filepath.c_str(), "wb");
  if (!file) {
    ERROR_MSG("FrameSaver: Failed to open file for writing: {}", filepath);
    av_packet_free(&pkt);
    if (convert_frame) av_frame_free(&convert_frame);
    avcodec_free_context(&codec_ctx);
    return false;
  }

  fwrite(pkt->data, 1, pkt->size, file);
  fclose(file);

  /// Cleanup
  av_packet_free(&pkt);
  if (convert_frame) av_frame_free(&convert_frame);
  avcodec_free_context(&codec_ctx);

  return true;
}

bool FrameSaver::save_frame(const AVFrame *frame,
        long long                          stream_id,
        uint64_t                           frame_index,
        const std::string                 &stage) {

  /// Check if saving is enabled for this stage
  if (stage == "decoded" && !save_decoded_enabled_) return false;
  if (stage == "processed" && !save_processed_enabled_) return false;

  /// Check interval
  if (frame_index % static_cast<uint64_t>(save_interval_) != 0) {
    return false;
  }

  /// Validate frame
  if (!frame || frame->width <= 0 || frame->height <= 0) {
    WARN_MSG("FrameSaver: Invalid frame for stream {}", stream_id);
    return false;
  }

  /// Build directory path: {base}/frames/{stage}/stream_{id}/
  std::string dir_path = fmt::format("{}/frames/{}/stream_{}", base_output_dir_, stage, stream_id);

  /// Ensure directory exists
  if (!ensure_directory_exists(dir_path)) {
    return false;
  }

  /// Build file path with zero-padded frame index
  std::string filepath = fmt::format("{}/frame_{:08d}.jpg", dir_path, frame_index);

  /// Encode and save
  if (!encode_and_save_jpeg(frame, filepath)) {
    ERROR_MSG("FrameSaver: Failed to save frame to '{}'", filepath);
    return false;
  }

  // Use DEBUG instead of TRACE to avoid unused var warnings if TRACE isn't defined here
  DEBUG_MSG("FrameSaver: Saved {} frame {} for stream {} to '{}'",
          stage,
          frame_index,
          stream_id,
          filepath);

  return true;
}
