/**
 * @file types.h
 * @brief Defines core data structures and custom types for the application.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#ifdef __linux__
#include <pthread.h>
#endif

extern "C" {
#include <libavutil/rational.h>
}

/// Forward declarations of FFmpeg structs
struct AVPacket;
struct AVFrame;
struct AVCodecContext;
struct AVDictionary;
struct AVFormatContext;
struct AVFilterGraph;
struct SwsContext;

// **---------- RAII WRAPPERS for FFMPEG objs ----------**

/**
 * @struct AVPacketDeleter
 * @brief Custom deleter for `AVPacket`
 */
struct AVPacketDeleter {
  void operator()(AVPacket *p) const;
};

/**
 * @struct AVFrameDeleter
 * @brief Custom deleter for `AVFrame`
 */
struct AVFrameDeleter {
  void operator()(AVFrame *p) const;
};

/**
 * @struct AVCodecContextDeleter
 * @brief Custom deleter for `AVCodecContext`
 */
struct AVCodecContextDeleter {
  void operator()(AVCodecContext *p) const;
};

/**
 * @struct AVFormatContextCloseInputDeleter
 * @brief Custom deleter for `AVFormatContext` that calls `avformat_close_input`
 * @details This is necessary because `avformat_close_input` takes an `AVFormatContext**` and
 *          cannot be used directly as a `std::unique_ptr` deleter.
 */
struct AVFormatContextCloseInputDeleter {
  void operator()(AVFormatContext *f) const;
};

/**
 * @struct AVFilterGraphDeleter
 * @brief Custom deleter for `AVFilterGraph`
 */
struct AVFilterGraphDeleter {
  void operator()(AVFilterGraph *g) const;
};

/**
 * @typedef UniqueAVPacket
 * @brief RAII wrapper for `AVPacket`
 */
using UniqueAVPacket = std::unique_ptr<AVPacket, AVPacketDeleter>;

/**
 * @typedef UniqueAVFrame
 * @brief RAII wrapper for `AVFrame`
 */
using UniqueAVFrame = std::unique_ptr<AVFrame, AVFrameDeleter>;

/**
 * @typedef UniqueAVCodecContext
 * @brief RAII wrapper for `AVCodecContext`
 */
using UniqueAVCodecContext = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

/**
 * @typedef UniqueAVFormatContext
 * @brief RAII wrapper for `AVFormatContext`
 */
using UniqueAVFormatContext = std::unique_ptr<AVFormatContext, AVFormatContextCloseInputDeleter>;

/**
 * @typedef UniqueAVFilterGraph
 * @brief RAII wrapper for `AVFilterGraph`
 */
using UniqueAVFilterGraph = std::unique_ptr<AVFilterGraph, AVFilterGraphDeleter>;

// **---------- CONFIGURATION STRUCTS ----------**

/**
 * @struct ScaleConfig
 * @brief Hold the values used during SCALING the frame during processing
 */
struct ScaleConfig {
  int  width  = 0;
  int  height = 0;
  bool operator==(const ScaleConfig &other) const;
};

/**
 * @struct CropConfig
 * @brief Hold the value used during CROPPING the frame during processing
 */
struct CropConfig {
  int  x      = 0;
  int  y      = 0;
  int  width  = 0;
  int  height = 0;
  bool operator==(const CropConfig &other) const;
};

/**
 * @struct ProcessingConfig
 * @brief Holds `ScaleConfig` and `CropConfig` struct
 */
struct ProcessingConfig {
  ScaleConfig scale;
  CropConfig  crop;
  bool        operator==(const ProcessingConfig &other) const;
};

/**
 * @struct EventConfig
 * @brief Holds the parameters for a single processing event, including its target
 * frame rate and the batch size for publishing.
 */
struct EventConfig {
  std::string name;
  double      target_fps = 1.0;
  int         batch_size = 8; //< Default batch size if not specified
  bool        operator==(const EventConfig &other) const;
};

/**
 * @struct StreamConfig
 * @brief Holds all params for a single video stream
 */
struct StreamConfig {
  int                      id;
  std::string              url;
  double                   target_fps;
  bool                     wait_for_keyframe;
  bool                     enabled;
  ProcessingConfig         processing;
  std::vector<EventConfig> events;
  bool                     operator==(const StreamConfig &other) const;
};

/**
 * @struct GlobalConfig
 * @brief Holds the entire app configuration
 */
struct GlobalConfig {
  std::string               log_level_str = "INFO";
  std::vector<StreamConfig> stream_configs;
  bool                      operator==(const GlobalConfig &other) const;
};

// **---------- DATA TRANSFER OBJECTS ----------**

/**
 * @struct DecodedFrame
 * @brief A container for a decoded frame and its completion metadata.
 *
 * @details This is the object that gets pushed to the central processing fifo.
 *          It contains everything a downstream consumer needs to process the frame without
 *          needing to look up the original stream context.
 */
struct DecodedFrame {
  UniqueAVFrame frame;
  StreamConfig  source_config; ///< A copy of the config at the time of capture
  EventConfig   event_config;
  AVRational    time_base;
};

// **---------- THREAD NAMING UTILITY ----------**

/**
 * @brief Sets the name of the current running thread.
 * @details This is a platform-specific operation. On Linux, it uses pthread_setname_np.
 * @param name The desired name for the thread.
 * @note The name is truncated to 15 characters to comply with the kernel's limit.
 */
inline void set_current_thread_name(const std::string &name) {
#ifdef __linux__
  /// pthread_setname_np has a 16-byte limit for the name (including null terminator)
  std::string truncated_name = name.substr(0, 15);
  pthread_setname_np(pthread_self(), truncated_name.c_str());
#endif
}