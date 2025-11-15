/**
 * @file config_manager.cpp
 * @brief Implements the ConfigManager class.
 */
#include "config_manager.h"

#include <condition_variable>
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <simdjson.h>
#include <stdexcept>
#include <system_error>

#include "utils/logger.h"

namespace {
/**
 * @class CurlHandle
 * @brief A simple RAII wrapper for a CURL handle.
 * @details Ensures that `curl_easy_cleanup` is called automatically when the handle goes
 *          out of the scope, preventing resource leaks even in the case of exceptions.
 */
class CurlHandle {
public:
  /**
   * @brief Initializes the CURL handle
   */
  CurlHandle()
      : handle_(curl_easy_init()) {}

  /**
   * @brief Cleans up the CURL handle
   */
  ~CurlHandle() {
    if (handle_) {
      curl_easy_cleanup(handle_);
    }
  }

  /// Disable copy semantics
  CurlHandle(const CurlHandle &)            = delete;
  CurlHandle &operator=(const CurlHandle &) = delete;

  /// Enable Move semantics
  CurlHandle(CurlHandle &&other) noexcept
      : handle_(other.handle_) {
    other.handle_ = nullptr;
  }
  CurlHandle &operator=(CurlHandle &&other) noexcept {
    if (this != &other) {
      if (handle_) {
        curl_easy_cleanup(handle_);
      }
      handle_       = other.handle_;
      other.handle_ = nullptr;
    }
    return *this;
  }

  /**
   * @brief Provides access to the raw CURL handle.
   * @return The raw `CURL*` handle
   */
  CURL *get() const { return handle_; }

private:
  CURL *handle_;
};

/**
 * @brief Callback function for libcurl to write received data into a std::string
 * @param contents Pointer to the data received
 * @param size Size of each data data item
 * @param nmemb Number of data items
 * @param userp Pointer to the `std::string` to append to
 * @return The total number of bytes handled
 */
size_t write_callback(void *contents, size_t size, size_t nmemb, std::string *userp) {
  if (userp == nullptr) {
    return 0;
  }
  try {
    userp->append(static_cast<char *>(contents), size * nmemb);
  } catch (const std::bad_alloc &) {
    /// Not much to do here, returning 0 will signal an error to curl
    return 0;
  }
  return size * nmemb;
}

/**
 * @brief Safely retrieves an environment variable
 * @param var The name of the environment variable
 * @param default_value The value to return if the variable is not set
 * @return The value of the environment variable or the default value
 */
std::string get_env(const char *var, const std::string &default_value) {
  const char *val = std::getenv(var);
  return val ? val : default_value;
}

} // namespace

ConfigManager::ConfigManager() {
  config_path_      = get_env("FP_CONFIG_PATH", "/etc/fp/configs/config.json");
  api_url_          = get_env("FP_CONFIG_API_URL", "");
  auto interval_str = get_env("FP_CONFIG_RELOAD_INTERVAL_S", "60");

  try {
    reload_interval_ = std::chrono::seconds(std::stoll(interval_str));
  } catch (const std::invalid_argument &e) {
    throw std::invalid_argument("Invalid format for FP_CONFIG_RELOAD_INTERVAL_S:" + interval_str);
  } catch (const std::out_of_range &e) {
    throw std::out_of_range("FP_CONFIG_RELOAD_INTERVAL_S is out of range: " + interval_str);
  }
}

ConfigManager::~ConfigManager() {
  if (reloader_thread_.joinable()) {
    reloader_thread_.request_stop();
  }
}

bool ConfigManager::load_initial_config() {
  GlobalConfig initial_config;
  if (!parse_config_json(initial_config)) {
    FAIL_MSG("Failed to parse initial JSON config at: {}", config_path_);
    return false;
  }
  std::lock_guard lock(config_mutex_);
  current_config_ = std::move(initial_config);
  return true;
}

void ConfigManager::start_reloader(OnUpdateCallback callback) {
  on_update_callback_ = std::move(callback);
  if (on_update_callback_) {
    GlobalConfig config_copy;
    {
      std::lock_guard lock(config_mutex_);
      config_copy = current_config_;
    }
    /// Immediately invoke callback with the initial config
    on_update_callback_(current_config_);
  }

  reloader_thread_ = std::jthread([this](std::stop_token st) { this->reloader_loop(st); });
  INFO_MSG("Config Manager started. Checking for updates every {}s.", reload_interval_);

  if (!api_url_.empty()) {
    INFO_MSG("API polling is ENABLED for URL: {}", api_url_);
  } else {
    INFO_MSG("API polling is DISABLED.");
  }
}

void ConfigManager::reloader_loop(std::stop_token st) {
  while (!st.stop_requested()) {
    /// Using a conditional variable for an uninterruptible wait
    /// This allows for a much faster shutdown if requested
    std::mutex       wait_mutex;
    std::unique_lock lock(wait_mutex);
    /// Added a simple lambda '[] { return false; }' as the required predicate.
    std::condition_variable_any().wait_for(lock, st, reload_interval_, [] { return false; });

    if (st.stop_requested()) {
      break;
    }

    DEBUG_MSG("Checking for configuration updates");

    // **---------- STEP-1 | Poll API if configured ----------**

    if (!api_url_.empty()) {
      if (auto api_json_opt = fetch_config_from_api()) {
        if (!update_local_config_from_api(*api_json_opt)) {
          ERROR_MSG("Failed to merge API config into local file. Will retry in next cycle.");
        }
      }
    }

    // **---------- STEP-2 | Read the local file ----------**

    GlobalConfig new_config;
    if (!parse_config_json(new_config)) {
      ERROR_MSG("Failed to parse config file during reload. Skipping update cycle");
      continue; // Wait for the next cycle
    }

    GlobalConfig config_to_notify;
    bool         needs_update = false;

    {
      std::lock_guard lock(config_mutex_);
      if (!(current_config_ == new_config)) {
        INFO_MSG("Configuration change detected. Applying updates...");
        current_config_  = std::move(new_config);
        config_to_notify = current_config_; // Make a copy for the callback
        needs_update     = true;
      }
    }

    // **---------- STEP-3 | IF UPDATE REQUIRED, INVOKE CALLBACK ----------**

    if (needs_update && on_update_callback_) {
      on_update_callback_(config_to_notify);
    }
  }
  INFO_MSG("Config reloader thread stopped.");
}

std::optional<std::string> ConfigManager::fetch_config_from_api() const {
  CurlHandle curl;

  if (!curl.get()) {
    ERROR_MSG("Failed to initialize libcurl handle.");
    return std::nullopt;
  }

  std::string read_buffer;
  curl_easy_setopt(curl.get(), CURLOPT_URL, api_url_.data());
  curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &read_buffer);
  curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L); // 10 secs timeout

  CURLcode res = curl_easy_perform(curl.get());
  if (res != CURLE_OK) {
    ERROR_MSG("curl_easy_perform() failed: {}", curl_easy_strerror(res));
    return std::nullopt;
  }

  long http_code = 0;
  curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);

  if (http_code != 200) {
    ERROR_MSG(
            "API request failed with HTTP status code: {}. Response: '{}'", http_code, read_buffer);
    return std::nullopt;
  }

  return read_buffer;
}

bool ConfigManager::update_local_config_from_api(const std::string &api_json_str) {
  simdjson::dom::parser  parser;
  simdjson::dom::element api_doc;
  if (auto err = parser.parse(api_json_str).get(api_doc); err) {
    ERROR_MSG("Failed to parse API JSON response: {}", simdjson::error_message(err));
    return false;
  }

  simdjson::dom::element local_doc;
  if (auto err = parser.load(config_path_.data()).get(local_doc); err) {
    ERROR_MSG("Failed to load local config file for merging: {}", simdjson::error_message(err));
    return false;
  }

  // **---------- MERGE LOGIC ----------**

  simdjson::dom::element final_global_settings = local_doc["global_settings"];
  simdjson::dom::element final_streams         = local_doc["streams"];
  bool                   updated               = false;

  if (api_doc["global_settings"].error() == simdjson::SUCCESS) {
    final_global_settings = api_doc["global_settings"];
    updated               = true;
    INFO_MSG("Updated 'global_settings' from API.");
  }
  if (api_doc["streams"].error() == simdjson::SUCCESS) {
    final_streams = api_doc["streams"];
    updated       = true;
    INFO_MSG("Updated 'streams' from API.");
  }

  if (!updated) {
    DEBUG_MSG("API response contained no new data to merge.");
    return true;
  }

  std::string new_json_content =
          "{\n  \"global_settings\": " + simdjson::minify(final_global_settings) +
          ",\n  \"streams\": " + simdjson::minify(final_streams) + "\n}";

  // **---------- ATOMIC WRITE ----------**

  /// Write to a temporary file in the same dir, then rename
  const std::filesystem::path temp_path = config_path_ + ".tmp";
  {
    std::ofstream out_file(temp_path);
    if (!out_file) {
      ERROR_MSG("Failed to open temporary config file for writing: {}", temp_path.string());
      return false;
    }
    out_file << new_json_content;
  }
  std::error_code errCode;
  std::filesystem::rename(temp_path, config_path_, errCode);
  if (errCode) {
    ERROR_MSG("Failed to atomically rename config file: {}. Error: {}",
            config_path_,
            errCode.message());
    std::filesystem::remove(temp_path, errCode);
    return false;
  }
  INFO_MSG("Successfully updated local config file from API.");
  return true;
}

bool ConfigManager::parse_config_json(GlobalConfig &config_out) const {
  simdjson::dom::parser  parser;
  simdjson::dom::element doc;
  if (auto err = parser.load(config_path_.data()).get(doc); err) {
    ERROR_MSG("Failed to parse '{}': {}", config_path_.data(), simdjson::error_message(err));
    return false;
  }

  try {
    std::string_view log_level_sv;
    if (doc["global_settings"]["log_level"].get_string().get(log_level_sv) == simdjson::SUCCESS) {
      config_out.log_level_str = log_level_sv;
    } else {
      WARN_MSG("Could not find 'log_level' in config, using default 'INFO'");
      config_out.log_level_str = "INFO";
    }
    /// Find out the stream array
    simdjson::dom::array streams_arr;
    if (auto err = doc["streams"].get_array().get(streams_arr); err) {
      ERROR_MSG("JSON parsing error: 'streams' field is missing or not an array in "
                "'{}'.",
              config_path_);
      return false;
    }
    config_out.stream_configs.clear();
    config_out.stream_configs.reserve(streams_arr.size());

    /// Iterate over each stream object in the array
    for (simdjson::dom::element stream_elem : streams_arr) {
      StreamConfig     cfg;
      int64_t          id_val;
      std::string_view url_val;

      /// Each required field (id, url) is checked for errors. If invalid,
      /// log a warning and skip this stream entry.
      if (auto err = stream_elem["id"].get_int64().get(id_val); err) {
        WARN_MSG("Parse error: stream 'id' is invalid. Skipping stream. Error: {}",
                simdjson::error_message(err));
        continue;
      }
      cfg.id = id_val;

      if (auto err = stream_elem["url"].get_string().get(url_val); err) {
        WARN_MSG("Parse error: stream 'url' is invalid. Skipping stream. Error: {}",
                simdjson::error_message(err));
        continue;
      }
      cfg.url = url_val;

      if (stream_elem["target_fps"].get_double().get(cfg.target_fps) != simdjson::SUCCESS) {
        cfg.target_fps = 1.0;
      }
      if (stream_elem["wait_for_keyframe"].get_bool().get(cfg.wait_for_keyframe) !=
              simdjson::SUCCESS) {
        cfg.wait_for_keyframe = false;
      }
      if (stream_elem["enabled"].get_bool().get(cfg.enabled) != simdjson::SUCCESS) {
        cfg.enabled = false;
      }
      /// Parse the nested 'processing' object
      simdjson::dom::object proc_obj;
      if (stream_elem["processing"].get_object().get(proc_obj) == simdjson::SUCCESS) {
        simdjson::dom::object scale_obj;
        if (proc_obj["scale"].get_object().get(scale_obj) == simdjson::SUCCESS) {
          int64_t w, h;
          if (scale_obj["width"].get_int64().get(w) == simdjson::SUCCESS)
            cfg.processing.scale.width = w;
          if (scale_obj["height"].get_int64().get(h) == simdjson::SUCCESS)
            cfg.processing.scale.height = h;
        }
        simdjson::dom::object crop_obj;
        if (proc_obj["crop"].get_object().get(crop_obj) == simdjson::SUCCESS) {
          int64_t x, y, w, h;
          if (crop_obj["x"].get_int64().get(x) == simdjson::SUCCESS) cfg.processing.crop.x = x;
          if (crop_obj["y"].get_int64().get(y) == simdjson::SUCCESS) cfg.processing.crop.y = y;
          if (crop_obj["width"].get_int64().get(w) == simdjson::SUCCESS)
            cfg.processing.crop.width = w;
          if (crop_obj["height"].get_int64().get(h) == simdjson::SUCCESS)
            cfg.processing.crop.height = h;
        }
      }
      simdjson::dom::array events_arr;
      if (stream_elem["events"].get_array().get(events_arr) == simdjson::SUCCESS) {
        for (simdjson::dom::element event_elem : events_arr) {
          EventConfig      event_cfg;
          std::string_view event_name;
          if (event_elem["name"].get_string().get(event_name) == simdjson::SUCCESS) {
            event_cfg.name = event_name;
          }

          if (event_elem["target_fps"].get_double().get(event_cfg.target_fps) !=
                  simdjson::SUCCESS) {
            event_cfg.target_fps = 1.0;
          }

          int64_t batch_val = 8; ///< Default value
          event_elem["batch_size"].get_int64().get(batch_val);
          event_cfg.batch_size = static_cast<int>(batch_val);

          cfg.events.push_back(std::move(event_cfg));
        }
      }

      if (cfg.url.empty()) {
        WARN_MSG("Stream with id {} has an empty URL. It will be ignored.", cfg.id);
        continue;
      }
      config_out.stream_configs.push_back(std::move(cfg));
    }
  } catch (const simdjson::simdjson_error &e) {
    ERROR_MSG("Critical simdjson error parsing '{}': {}", config_path_.data(), e.what());
    return false;
  }
  return true;
}