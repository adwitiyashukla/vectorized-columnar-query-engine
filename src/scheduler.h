#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace vcqe {

class MorselQueue {
public:
  explicit MorselQueue(size_t total) : total_(total) {}
  bool Next(size_t* index) {
    const size_t taken = next_.fetch_add(1, std::memory_order_relaxed);
    if (taken >= total_) {
      return false;
    }
    *index = taken;
    return true;
  }

private:
  std::atomic<size_t> next_{0};
  size_t total_ = 0;
};

class ThreadPool {
public:
  explicit ThreadPool(size_t workers);
  ~ThreadPool();

  size_t size() const {
    return workers_.size() + 1;
  }
  void Run(size_t tasks, const std::function<void(size_t)>& body);

private:
  void Loop();

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable done_;
  const std::function<void(size_t)>* body_ = nullptr;
  size_t pending_ = 0;
  size_t next_task_ = 0;
  size_t task_count_ = 0;
  uint64_t generation_ = 0;
  bool stop_ = false;
  std::exception_ptr failure_;
};

}
