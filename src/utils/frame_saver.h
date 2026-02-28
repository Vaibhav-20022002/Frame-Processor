/**
 * @file frame_saver.h
 * @brief Utility class for saving video frames to disk with configurable intervals.
 *
 * @details This utility enables saving frames at two points in the pipeline:
 *  1. After decoding (raw frames from RTSP streams)
 *  2. After processing (frames after crop/scale transformations)
 *
 * Features:
 *  - Automatic directory creation per stream
 *  - Sequential file naming with zero-padded counters
 *  - Configurable save interval (save every Nth frame)
 *  - Thread-safe operations
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct AVFrame; ///< Forward declaration

/**
 * @class FrameSaver
 * @brief Saves video frames to disk as JPEG images with configurable intervals.
 *
 * @attention Thread Safety:
 *  - Directory creation is protected by mutex
 *  - Frame counters are atomic per stream
 *  - Encoding is stateless and thread-safe
 */
class FrameSaver {
public:
  /**
   * @brief Constructs a FrameSaver with configuration from environment variables.
   *
   * @note Reads the following environment variables:
   *  - FP_SAVE_DECODED_FRAMES (default: false)
   *  - FP_SAVE_PROCESSED_FRAMES (default: false)
   *  - FP_SAVE_FRAME_INTERVAL (default: 1)
   *  - FP_SAVED_FRAMES_OUTPUT_DIR (default: /tmp/output)
   */
  FrameSaver();

  ~FrameSaver() = default;

  /// Non-copyable, non-movable (owns atomic counters)
  FrameSaver(const FrameSaver &)            = delete;
  FrameSaver &operator=(const FrameSaver &) = delete;
  FrameSaver(FrameSaver &&)                 = delete;
  FrameSaver &operator=(FrameSaver &&)      = delete;

  /**
   * @brief Saves a frame to disk if conditions are met.
   *
   * @param frame       The AVFrame to save (YUV420P or BGR24 format supported).
   * @param stream_id   Stream identifier for directory organization.
   * @param frame_index Current frame index (used for interval check and naming).
   * @param stage       "decoded" or "processed" for directory segmentation.
   *
   * @return true if frame was saved, false if skipped (interval) or failed.
   */
  bool save_frame(const AVFrame *frame,
          long long              stream_id,
          uint64_t               frame_index,
          const std::string     &stage);

  /// Configuration accessors
  [[nodiscard]] bool is_decoded_enabled() const { return save_decoded_enabled_; }
  [[nodiscard]] bool is_processed_enabled() const { return save_processed_enabled_; }
  [[nodiscard]] int  get_save_interval() const { return save_interval_; }

private:
  std::string base_output_dir_;
  int         save_interval_;
  bool        save_decoded_enabled_;
  bool        save_processed_enabled_;

  /// Thread-safe directory creation tracking
  std::mutex                      dir_creation_mutex_;
  std::unordered_set<std::string> created_dirs_;

  /**
   * @brief Ensures a directory exists, creating it recursively if needed.
   * @param path The directory path to create.
   * @return true if directory exists/was created, false on failure.
   */
  bool ensure_directory_exists(const std::string &path);

  /**
   * @brief Encodes an AVFrame to JPEG and saves to disk.
   *
   * @param frame    The frame to encode (will be converted to RGB if needed).
   * @param filepath Full path to save the JPEG file.
   * @return true on success, false on failure.
   */
  bool encode_and_save_jpeg(const AVFrame *frame, const std::string &filepath);
};
