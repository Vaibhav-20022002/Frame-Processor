/**
 * @file config_manager.h
 * @brief ConfigManager: dynamic configuration loader and reloader.
 *
 * @details Reads configuration from a local JSON file and optionally polls a
 *          remote API. Exposes a thread-safe API to load the initial configuration and
 *          to start a background reloader that notifies the application via a callback
 *          when the configuration changes.
 */
#pragma once
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_set>

#include "utils/types.h"

#define RELOAD_INTERVAL_CONFIG_API_S 60 ///< Time (in secs) for API polling

class ConfigManager {
public:
  /**
   * @brief A callback function type that is invoked when config is updated.
   * @param config A const reference to the newly loaded `GlobalConfig` object
   * @note The callback is guaranteed to be called with a valid config object.
   */
  using OnUpdateCallback = std::function<void(const GlobalConfig &)>;

  /**
   * @brief Constructs the ConfigManager.
   * @note Reads environment variables to determine default paths and reload interval.
   */
  ConfigManager();
  ~ConfigManager();

  /**
   * @brief Loads the initial configuration from the specified file path.
   * @return `true` if the initial configuration was successfully loaded and parsed, `false`
   *         otherwise.
   * @note On failure, an error is logged.
   */
  bool load_initial_config();

  /**
   * @brief Starts the background reloader thread.
   * @param callback The function to be called when the configuration is updated.
   * @pre `load_initial_config()` must have been called successfully.
   * @post The background reloader thread is running.
   */
  void start_reloader(OnUpdateCallback callback);

private:
  /**
   * @brief The main loop for the reloader thread
   * @note This loop handles both API polling and file monitoring.
   */
  void reloader_loop(std::stop_token stop_token);

  /**
   * @brief Fetches a JSON configuration from a remote URL
   * @returns An optional string containing the JSON response body.
   *          Returns `std::nullopt` on any failure (network, HTTP error, etc.)
   */
  std::optional<std::string> fetch_config_from_api() const;

  /**
   * @brief Merges changes from an API response into the local config file
   * @param api_json_str The JSON string received from the API.
   * @returns `true` if the local config was successfully updated, `false` otherwise.
   */
  bool update_local_config_from_api(const std::string &api_json_str);

  /**
   * @brief Parses the local JSON config file into a `GlobalConfig` struct.
   * @param[out] config_out The struct to be populated with the parsed configuration data.
   * @return `true` on successful parsing, `false` if a critical error occurs (e.g.,
   *         file not found or completely invalid JSON).
   * @note This function is `const` because it does not modify the state of the ConfigManager.
   */
  bool parse_config_json(GlobalConfig &configs_out) const;

  /**
   * @brief Determines if this instance owns a stream based on consistent hashing.
   * @param stream_id The stream ID to check
   * @return True if this instance should process the stream
   */
  bool owns_stream(long long stream_id) const {
    if (total_instances_ <= 1) return true; //< Single instance mode
    return (stream_id % total_instances_) == (instance_slot_ % total_instances_);
  }

  /**
   * @brief Checks if a stream should be processed based on whitelist.
   * @param stream_id The ID to check
   * @return True if stream should be processed (whitelist empty or ID in whitelist)
   */
  bool is_stream_whitelisted(long long stream_id) const {
    if (!has_stream_whitelist_) return true; //< No whitelist = allow all
    return stream_id_whitelist_.count(stream_id) > 0;
  }

  std::string          config_path_;
  std::string          api_url_;
  std::chrono::seconds reload_interval_{RELOAD_INTERVAL_CONFIG_API_S};
  GlobalConfig         current_config_;

  /// Horizontal Scaling (consistent hashing)
  int instance_slot_;   //< 0-indexed instance slot (from FP_INSTANCE_SLOT)
  int total_instances_; //< Total replica count (from FP_TOTAL_INSTANCES)
  int max_streams_;     //< Max streams per instance (0 = unlimited)

  /// Stream ID whitelist filtering (FP_STREAM_IDS)
  std::unordered_set<long long> stream_id_whitelist_; //< Set of allowed stream IDs (empty = all)
  bool has_stream_whitelist_ = false;                 //< True if FP_STREAM_IDS was explicitly set

  mutable std::mutex config_mutex_;
  OnUpdateCallback   on_update_callback_;
  std::jthread       reloader_thread_;
};