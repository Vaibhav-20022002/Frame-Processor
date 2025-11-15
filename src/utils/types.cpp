/**
 * @file types.cpp
 * @brief Implements methods and deleters for data structures in types.h.
 */
#include "types.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libswscale/swscale.h>
}
// **---------- DELETER IMPLEMENTATIONS ----------**

void AVPacketDeleter::operator()(AVPacket *p) const {
  av_packet_free(&p);
}

void AVFrameDeleter::operator()(AVFrame *p) const {
  av_frame_free(&p);
}

void AVCodecContextDeleter::operator()(AVCodecContext *p) const {
  avcodec_free_context(&p);
}

void AVFormatContextCloseInputDeleter::operator()(AVFormatContext *f) const {
  if (f) {
    avformat_close_input(&f);
  }
}

void AVFilterGraphDeleter::operator()(AVFilterGraph *g) const {
  if (g) {
    avfilter_graph_free(&g);
  }
}

// **---------- EQUALITY OPERATOR IMPLEMENTATIONS ----------**

bool ScaleConfig::operator==(const ScaleConfig &other) const {
  return width == other.width && height == other.height;
}

bool CropConfig::operator==(const CropConfig &other) const {
  return x == other.x && y == other.y && width == other.width && height == other.height;
}

bool ProcessingConfig::operator==(const ProcessingConfig &other) const {
  return scale == other.scale && crop == other.crop;
}

bool EventConfig::operator==(const EventConfig &other) const {
  return name == other.name && target_fps == other.target_fps && batch_size == other.batch_size;
}

bool StreamConfig::operator==(const StreamConfig &other) const {
  return id == other.id && url == other.url && wait_for_keyframe == other.wait_for_keyframe &&
          enabled == other.enabled && events == other.events && processing == other.processing;
}

bool GlobalConfig::operator==(const GlobalConfig &other) const {
  return log_level_str == other.log_level_str && stream_configs == other.stream_configs;
}