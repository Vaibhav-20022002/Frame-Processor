/**
 * @file publisher.cpp
 * @brief Implements the Publisher class with robust frame format handling and tightly-packed output.
 */

#include "publisher.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "utils/logger.h"
#include "utils/redis_client.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {
std::string get_env(const char *var, const std::string &default_value) {
  const char *val = std::getenv(var);
  return val ? val : default_value;
}

/// Reasonable upper-bound for a single packed frame (protects against insane sizes)
constexpr size_t MAX_FRAME_BYTES = 100ull * 1024ull * 1024ull; // 100 MB per frame safety

#pragma pack(push, 1)
/**
 * @struct FrameHeader
 * @brief A simplified, robust header. The consumer can derive plane sizes from this.
 */
struct FrameHeader {
  int64_t  pts;
  uint32_t width;
  uint32_t height;
};
#pragma pack(pop)

/// Convert 'src' to an allocated AVFrame in AV_PIX_FMT_YUV420P. Returns empty on failure.
/// The returned frame has a contiguous buffer allocated (av_image_alloc) so .data/.linesize are
/// valid.
static UniqueAVFrame convert_frame_to_i420(const AVFrame *src) {
  if (!src) return nullptr;
  if (src->format == AV_PIX_FMT_YUV420P) return nullptr; // no-op — caller will use original

  if (src->format == AV_PIX_FMT_NONE) return nullptr;

  AVFrame *dst_raw = av_frame_alloc();
  if (!dst_raw) return nullptr;
  UniqueAVFrame dst(dst_raw);

  dst->format = AV_PIX_FMT_YUV420P;
  dst->width  = src->width;
  dst->height = src->height;

  int ret =
          av_image_alloc(dst->data, dst->linesize, dst->width, dst->height, AV_PIX_FMT_YUV420P, 1);
  if (ret < 0) {
    ERROR_MSG("av_image_alloc failed: {}", ret);
    return nullptr;
  }

  SwsContext *sws = sws_getContext(src->width,
          src->height,
          static_cast<AVPixelFormat>(src->format),
          dst->width,
          dst->height,
          AV_PIX_FMT_YUV420P,
          SWS_BILINEAR,
          nullptr,
          nullptr,
          nullptr);
  if (!sws) {
    ERROR_MSG("sws_getContext failed for conversion.");
    av_freep(&dst->data[0]);
    return nullptr;
  }

  int rows = sws_scale(sws, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
  sws_freeContext(sws);

  if (rows != src->height) {
    ERROR_MSG("sws_scale wrote {} rows (expected {}).", rows, src->height);
    av_freep(&dst->data[0]);
    return nullptr;
  }

  /// copy pts
  dst->pts = src->pts;
  return dst;
}

/// Copy a plane into dest line-by-line (removes any source stride).
static void copy_plane_remove_stride(uint8_t *dest,
        const uint8_t                        *src,
        int                                   src_linesize,
        int                                   row_bytes,
        int                                   rows) {
  for (int r = 0; r < rows; ++r) {
    memcpy(dest + static_cast<size_t>(r) * row_bytes,
            src + static_cast<size_t>(r) * src_linesize,
            row_bytes);
  }
}

} // namespace

Publisher::Publisher(std::shared_ptr<fifo<DecodedFrame>> frame_queue)
    : processed_frame_queue_(std::move(frame_queue)) {
  redis_host_ = get_env("FP_REDIS_HOST", "");
  if (redis_host_.empty()) {
    WARN_MSG("FP_REDIS_HOST not set. Publisher is DISABLED.");
    return;
  }

  redis_port_            = std::stoi(get_env("FP_REDIS_PORT", "6379"));
  redis_queue_base_name_ = get_env("FP_REDIS_QUEUE_BASE_NAME", "fp_batches");
  is_enabled_            = true;

  INFO_MSG("Publisher is ENABLED. Target Redis: {}:{}. Queue base name: '{}'.",
          redis_host_,
          redis_port_,
          redis_queue_base_name_);

  batching_thread_ = std::jthread([this](std::stop_token st) { this->batching_loop(st); });

  int nupublisher_threads_ = std::stoi(get_env("FP_PUBLISHER_THREADS", "4"));
  nupublisher_threads_     = std::max(1, nupublisher_threads_);
  for (int i = 0; i < nupublisher_threads_; ++i) {
    publisher_threads_.emplace_back([this, i](std::stop_token st) { this->publisher_loop(st, i); });
  }
}

Publisher::~Publisher() {
  stop();
}

void Publisher::stop() {
  if (!is_enabled_) return;
  INFO_MSG("Stopping publisher...");

  processed_frame_queue_->stop();
  batching_thread_ = {};
  batch_queue_.stop();
  publisher_threads_.clear();

  INFO_MSG("Publisher stopped.");
}

/**
 * @brief This thread's only job is to efficiently serialize frames into batches
 * by performing a row-by-row copy to remove memory padding.
 */
void Publisher::batching_loop(std::stop_token st) {
  set_current_thread_name("batcher");
  INFO_MSG("Frame batching thread started.");

  using BatchKey = std::pair<long long, std::string>;
  std::map<BatchKey, FrameBatch> current_batches;

  while (!st.stop_requested()) {
    auto frame_opt = processed_frame_queue_->wait_and_pop();
    if (!frame_opt) break;

    DecodedFrame   d_frame    = std::move(*frame_opt);
    const auto    &stream_cfg = d_frame.source_config;
    const auto    &event_cfg  = d_frame.event_config;
    const AVFrame *frame      = d_frame.frame.get();

    BatchKey key = {stream_cfg.id, event_cfg.name};
    if (current_batches.find(key) == current_batches.end()) {
      FrameBatch new_batch;
      new_batch.stream_id  = stream_cfg.id;
      new_batch.event_name = event_cfg.name;
      new_batch.data_buffer.reserve(4 * 1024 * 1024);
      current_batches[key] = std::move(new_batch);
    }

    FrameBatch &batch = current_batches.at(key);

    // **---- Row-by-Row Serialization to Remove Padding ----**
    const uint32_t w                     = static_cast<uint32_t>(frame->width);
    const uint32_t h                     = static_cast<uint32_t>(frame->height);
    const size_t   y_plane_size          = static_cast<size_t>(w) * h;
    const size_t   uv_plane_size         = static_cast<size_t>(w / 2) * (h / 2);
    const size_t   total_frame_data_size = y_plane_size + uv_plane_size + uv_plane_size;
    const size_t   total_entry_size      = sizeof(FrameHeader) + total_frame_data_size;

    size_t current_buffer_size = batch.data_buffer.size();
    batch.data_buffer.resize(current_buffer_size + total_entry_size);
    uint8_t *dest_ptr = batch.data_buffer.data() + current_buffer_size;

    /// 1. Copy Header
    FrameHeader header{.pts = frame->pts, .width = w, .height = h};
    memcpy(dest_ptr, &header, sizeof(FrameHeader));
    dest_ptr += sizeof(FrameHeader);

    /// 2. Copy Y plane, row by row, to strip padding
    for (uint32_t i = 0; i < h; ++i) {
      memcpy(dest_ptr + i * w, frame->data[0] + i * frame->linesize[0], w);
    }
    dest_ptr += y_plane_size;

    /// 3. Copy U plane, row by row
    for (uint32_t i = 0; i < h / 2; ++i) {
      memcpy(dest_ptr + i * (w / 2), frame->data[1] + i * frame->linesize[1], w / 2);
    }
    dest_ptr += uv_plane_size;

    /// 4. Copy V plane, row by row
    for (uint32_t i = 0; i < h / 2; ++i) {
      memcpy(dest_ptr + i * (w / 2), frame->data[2] + i * frame->linesize[2], w / 2);
    }

    ++batch.frame_count;

    if (batch.frame_count >= event_cfg.batch_size) {
      DEBUG_MSG("Batch for stream {} event '{}' is full. Pushing to network.",
              batch.stream_id,
              batch.event_name);
      batch_queue_.push(std::move(batch));
      current_batches.erase(key);
    }
  }

  /// Flush partial batches
  for (auto &p : current_batches) {
    FrameBatch &b = p.second;
    if (b.frame_count > 0) {
      DEBUG_MSG("Flushing partial batch for stream {} event '{}' ({} frames) on exit.",
              b.stream_id,
              b.event_name,
              b.frame_count);
      batch_queue_.push(std::move(b));
    }
  }

  INFO_MSG("Frame batching thread finished.");
}

void Publisher::publisher_loop(std::stop_token st, int publisher_id) {
  set_current_thread_name(fmt::format("publisher_{}", publisher_id));
  INFO_MSG("Publisher thread {} started.", publisher_id);

  std::unique_ptr<RedisConnection> redis_conn =
          std::make_unique<RedisConnection>(redis_host_, redis_port_);

  while (!st.stop_requested()) {
    auto batch_opt = batch_queue_.wait_and_pop();
    if (!batch_opt) break;

    FrameBatch batch = std::move(*batch_opt);

    if (!redis_conn || !redis_conn->is_valid()) {
      redis_conn = std::make_unique<RedisConnection>(redis_host_, redis_port_);
      if (!redis_conn->is_valid()) {
        WARN_MSG("Publisher {}: cannot connect to Redis at {}:{}. Re-queueing batch and retrying "
                 "later.",
                publisher_id,
                redis_host_,
                redis_port_);
        batch_queue_.push(std::move(batch));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
    }

    redisContext *ctx = redis_conn->get();
    if (!ctx) {
      ERROR_MSG("Publisher {}: redis context unexpectedly null. Re-queueing batch.", publisher_id);
      batch_queue_.push(std::move(batch));
      redis_conn.reset();
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    std::string redis_key = fmt::format("{}:{}", redis_queue_base_name_, batch.stream_id);

    DEBUG_MSG("Publisher {} sending batch for stream {} event '{}' to Redis stream '{}'.",
            publisher_id,
            batch.stream_id,
            batch.event_name,
            redis_key);

    redisReply *reply = static_cast<redisReply *>(redisCommand(ctx,
            "XADD %s * event %b data %b",
            redis_key.c_str(),
            batch.event_name.empty() ? "" : batch.event_name.c_str(),
            static_cast<size_t>(batch.event_name.size()),
            batch.data_buffer.data(),
            static_cast<size_t>(batch.data_buffer.size())));

    if (reply == nullptr) {
      const char *err = ctx->errstr ? ctx->errstr : "(no errstr)";
      ERROR_MSG("Redis XADD command failed for key '{}': {}", redis_key, err);
      redis_conn.reset();
      batch_queue_.push(std::move(batch));
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    } else {
      DEBUG_MSG("Successfully published batch to stream {}", redis_key);
      freeReplyObject(reply);
    }
  }

  INFO_MSG("Publisher thread {} finished.", publisher_id);
}
