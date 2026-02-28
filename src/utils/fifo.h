/**
 * @file fifo.h
 * @brief Defines a stoppable, unbounded, thread-safe queue with a warning threshold.
 */
#pragma once
#include <condition_variable>
#include <list>
#include <mutex>
#include <optional>
#include <queue>

#include "utils/logger.h"

using namespace std::chrono_literals;

/**
 * @class fifo
 * @brief A thread-safe First-In-First-Out (`FIFO`) queue for inter-thread communication.
 * @tparam T Type of data held in the queue.
 */
template<typename T> class fifo {
public:
  /**
   * @brief Constructs the queue.
   * @param warning_threshold The "High Water Mark". If the queue size exceeds this,
   *        a warning is logged. This helps detect if consumers (e.g., Publisher) are
   *        falling behind producers (e.g., StreamIO).
   */
  explicit fifo(size_t warning_threshold, std::string name = "unnamed")
      : warning_threshold_(warning_threshold)
      , name_(std::move(name)) {}

  /// Queue cannot be copied
  fifo(const fifo &)            = delete;
  fifo &operator=(const fifo &) = delete;

  /**
   * @brief Pushes an item into the queue.
   * @param value The item to push (moved into the queue).
   */
  void push(T value) {
    std::lock_guard lock(mtx_);
    if (stopped_) return; //< Do not accept new work if stopped

    queue_.push(std::move(value));
    ++total_pushed_;
    if (queue_.size() > peak_size_) peak_size_ = queue_.size();

    // **---- High Water Mark Warning ----**
    if (queue_.size() > warning_threshold_) {
      /// Rate limit the warning to avoid log flooding (once per 10s)
      auto now = std::chrono::steady_clock::now();
      if (now - last_warning_time_ > 10s) {
        WARN_MSG("fifo '{}' has exceeded its warning threshold of {}. Current size: {}. This "
                 "indicates "
                 "a downstream consumer is blocked or too slow, which may lead to an OOM crash.",
                name_,
                warning_threshold_,
                queue_.size());
        last_warning_time_ = now;
      }
    }

    cv_.notify_one(); //< Wake up one waiting consumer
  }

  /**
   * @brief Pops an item from the queue, blocking if necessary.
   * @return `std::optional` containing the item, or `std::nullopt` if queue is stopped/empty.
   */
  std::optional<T> wait_and_pop() {
    std::unique_lock lock(mtx_);
    /// Wait until data is available OR the queue is explicitly stopped
    cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });

    if (stopped_ && queue_.empty()) return std::nullopt;

    T value = std::move(queue_.front());
    queue_.pop();
    ++total_popped_;
    return std::make_optional(std::move(value));
  }

  /**
   * @brief Signals the queue to stop.
   * @note Unblocks all waiting threads immediately so they can exit cleanly.
   */
  void stop() {
    std::lock_guard lock(mtx_);
    stopped_ = true;
    cv_.notify_all();
  }

  /**
   * @brief Get current queue size (for debugging/monitoring)
   * @return Current number of items in the queue
   */
  size_t size() const {
    std::lock_guard lock(mtx_);
    return queue_.size();
  }

  /**
   * @struct QueueStats
   * @brief Snapshot of queue statistics.
   */
  struct QueueStats {
    size_t current_size;
    size_t peak_size;
    size_t total_pushed;
    size_t total_popped;
    size_t drop_count;
  };

  /**
   * @brief Retrieves and optionally resets queue statistics.
   * @param reset_peak If true, resets the peak_size counter after reading.
   * @return QueueStats struct containing current metrics.
   */
  QueueStats get_stats(bool reset_peak = true) {
    std::lock_guard lock(mtx_);
    QueueStats      stats;
    stats.current_size = queue_.size();
    stats.peak_size    = peak_size_;
    stats.total_pushed = total_pushed_;
    stats.total_popped = total_popped_;
    stats.drop_count   = drop_count_;

    if (reset_peak) peak_size_ = queue_.size();
    return stats;
  }

  /**
   * @brief Increments the drop counter (call this when a frame is dropped downstream).
   */
  void increment_drop_count() { drop_count_++; }

  /**
   * @brief Get queue name (for debugging/monitoring)
   * @return The name of this queue
   */
  const std::string &name() const { return name_; }

private:
  mutable std::mutex      mtx_;
  std::condition_variable cv_;
  /// @note Uses `std::list` as backing container instead of default `std::deque`.
  ///       This prevents memory fragmentation in high-churn scenarios where frames
  ///       are constantly pushed and popped. `std::deque` can retain memory even
  ///       after elements are removed, leading to apparent "leaks" under jemalloc.
  std::queue<T, std::list<T>>           queue_;
  size_t                                warning_threshold_;
  std::string                           name_;
  std::atomic<bool>                     stopped_{false};
  std::chrono::steady_clock::time_point last_warning_time_{};

  // **---- Metrics ----**

  size_t              peak_size_{0};
  std::atomic<size_t> total_pushed_{0};
  std::atomic<size_t> total_popped_{0};
  std::atomic<size_t> drop_count_{0};
};