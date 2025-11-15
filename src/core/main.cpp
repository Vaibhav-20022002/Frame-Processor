/**
 * @file main.cpp
 * @brief Main entry point, handling process management and app lifecycle.
 *
 * @details This file contains the complete logic for the app's startup. It forks into a parent
 * (monitor) and child (worker) process. The parent's sole job is to relaunch the child if it
 * crashes. The child process runs the core app, setting up all components and handling graceful
 * shutdown.
 */

#include <csignal>
#include <memory>
#include <sys/wait.h>
#include <unistd.h>

#include "../utils/fifo.h"
#include "../utils/logger.h"
#include "../utils/types.h"
#include "components/config_manager/config_manager.h"
#include "components/publisher/publisher.h"
#include "components/stream_io/stream_io_manager.h"
#include "components/worker/worker.h"

using namespace std::chrono_literals;

/// Forward declaration
int app_logic();

/// Global flag to signal a shutdown request from a signal handler.
volatile sig_atomic_t g_shutdown_request = 0;

/// Global flag for parent process shutdown
volatile sig_atomic_t g_parent_shutdown_request = 0;

/// Global PID of current child (for parent signal handler)
volatile pid_t g_child_pid = 0;

/**
 * @brief Signal handler for child process (SIGINT and SIGTERM).
 */
void child_signal_handler(int /*signum*/) {
  g_shutdown_request = 1;
}

/**
 * @brief Signal handler for parent process (SIGINT and SIGTERM).
 */
void parent_signal_handler(int signum) {
  g_parent_shutdown_request = 1;

  // Forward signal to child if it exists
  if (g_child_pid > 0) {
    kill(g_child_pid, signum);
  }
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

/**
 * @brief Determines the number of worker threads from the environment variable.
 * @return The number of threads to use
 * @note Default logic: cores - 2, with a minimum of 1.
 */
int get_worker_count() {
  int cores   = std::thread::hardware_concurrency();
  int workers = std::max(1, cores - 2);

  std::string ret = get_env("FP_NUM_WORKER_THREADS", std::to_string(workers));
  INFO_MSG("Determined worker count: {}", ret);
  return std::stoi(ret);
}

/**
 * @brief The core app logic that runs inside the child process
 */
int app_logic() {
  INFO_MSG("App child process started (PID: {}).", getpid());

  // Set up child-specific signal handlers
  signal(SIGINT, child_signal_handler);
  signal(SIGTERM, child_signal_handler);

  std::unique_ptr<ConfigManager> config_manager;
  try {
    config_manager = std::make_unique<ConfigManager>();
  } catch (const std::exception &e) {
    FAIL_MSG("Fatal error during ConfigManager initialization: {}", e.what());
    return EXIT_FAILURE;
  }

  if (!config_manager->load_initial_config()) {
    return EXIT_FAILURE;
  }

  const int16_t num_workers = get_worker_count();

  auto decode_frame_queue    = std::make_shared<fifo<DecodedFrame>>();
  auto processed_frame_queue = std::make_shared<fifo<DecodedFrame>>();

  auto stream_manager = std::make_unique<StreamIoManager>(decode_frame_queue);
  auto worker_manager =
          std::make_unique<Worker>(decode_frame_queue, processed_frame_queue, num_workers);
  auto publisher = std::make_unique<Publisher>(processed_frame_queue);

  auto update_callback = [&](const GlobalConfig &config) {
    LogLevel new_level = string_to_loglevel(config.log_level_str);
    G_LOG_LEVEL.store(new_level);
    INFO_MSG("Log level updated to: {}", config.log_level_str);
    stream_manager->update_streams(config.stream_configs);
  };

  config_manager->start_reloader(update_callback);

  worker_manager->start();

  while (!g_shutdown_request) {
    std::this_thread::sleep_for(1s);
  }

  INFO_MSG("Shutdown signal received. Stopping services...");
  /// Graceful shutdown order: Stop producers first, then consumers
  stream_manager->stop_all();
  worker_manager->stop();
  publisher->stop();
  INFO_MSG("App child process finished.");
  return EXIT_SUCCESS;
}

/**
 * @brief The app's main entry point
 */
int main() {
  // Set up parent signal handlers
  signal(SIGINT, parent_signal_handler);
  signal(SIGTERM, parent_signal_handler);

  pid_t pid = fork();

  if (pid < 0) {
    FAIL_MSG("Fatal: fork() failed");
    return EXIT_FAILURE;
  }

  if (pid == 0) {
    // Child process - reset signal handlers to avoid inheritance issues
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    _exit(app_logic());
  } else {
    // Parent process
    g_child_pid = pid;
    INFO_MSG("Watching child (PID {}).", pid);

    int status;
    while (!g_parent_shutdown_request) {
      pid_t res = waitpid(pid, &status, WNOHANG);

      if (res == 0) {
        // Child still running
        std::this_thread::sleep_for(2s);
      } else if (res == pid) {
        // Child terminated
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
          INFO_MSG("Child process exited normally. Exiting...");
          break;
        }

        // Check if parent received shutdown signal
        if (g_parent_shutdown_request) {
          INFO_MSG("Parent shutdown requested. Exiting...");
          break;
        }

        // Log termination reason and relaunch
        if (WIFEXITED(status)) {
          ERROR_MSG("Child exited with code {}. Relaunching...", WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
          ERROR_MSG("Child killed by signal {}. Relaunching...", WTERMSIG(status));
        } else {
          ERROR_MSG("Child terminated unexpectedly. Relaunching...");
        }

        // Fork new child
        pid = fork();
        if (pid == 0) {
          // Reset signal handlers for new child
          signal(SIGINT, SIG_DFL);
          signal(SIGTERM, SIG_DFL);
          _exit(app_logic());
        } else if (pid > 0) {
          g_child_pid = pid;
          INFO_MSG("Relaunched child with new PID: {}", pid);
        } else {
          FAIL_MSG("Failed to fork new child! Exiting...");
          break;
        }
      } else if (res < 0) {
        // waitpid failed
        if (errno == EINTR) {
          // Interrupted by signal, continue loop
          continue;
        }
        FAIL_MSG("waitpid failed with error: {}. Exiting...", strerror(errno));
        break;
      }
    }

    // Clean up child process if still running
    if (g_child_pid > 0) {
      INFO_MSG("Terminating child process (PID: {})...", g_child_pid);
      kill(g_child_pid, SIGTERM);

      // Wait up to 5 seconds for graceful shutdown
      for (int i = 0; i < 5 && kill(g_child_pid, 0) == 0; ++i) {
        std::this_thread::sleep_for(1s);
      }

      // Force kill if still alive
      if (kill(g_child_pid, 0) == 0) {
        INFO_MSG("Child didn't terminate gracefully, sending SIGKILL...");
        kill(g_child_pid, SIGKILL);
      }

      // Final wait to avoid zombie
      waitpid(g_child_pid, nullptr, 0);
    }
  }

  return 0;
}