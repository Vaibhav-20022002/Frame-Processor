/**
 * @file fifo.h
 * @brief Defines a simple, blocking, thread-safe queue
 */
#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

template<typename T> class fifo {
public:
  fifo()                        = default;
  fifo(const fifo &)            = delete;
  fifo &operator=(const fifo &) = delete;

  void push(T value) {
    std::lock_guard lock(mtx_);
    queue_.push(std::move(value));
    cv_.notify_one();
  }

  /**
   * @brief Waits until an item is available or the queue is stopped.
   * @return An std::optional containing the item, or std::nullopt if the
   * queue is stopped and empty.
   */
  std::optional<T> wait_and_pop() {
    std::unique_lock lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty() || stopped_; });

    if (stopped_ && queue_.empty()) {
      return std::nullopt;
    }

    T value = std::move(queue_.front());
    queue_.pop();
    return value;
  }

  /**
   * @brief Stops the queue, unblocking any waiting threads.
   */
  void stop() {
    std::lock_guard lock(mtx_);
    stopped_ = true;
    cv_.notify_all();
  }

private:
  mutable std::mutex      mtx_;
  std::queue<T>           queue_;
  std::condition_variable cv_;
  std::atomic<bool>       stopped_{false};
};