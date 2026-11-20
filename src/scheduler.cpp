#include "scheduler.h"

namespace vcqe {

ThreadPool::ThreadPool(size_t workers) {
  const size_t extra = workers > 1 ? workers - 1 : 0;
  workers_.reserve(extra);
  for (size_t i = 0; i < extra; ++i) {
    workers_.emplace_back([this] { Loop(); });
  }
}

ThreadPool::~ThreadPool() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    stop_ = true;
  }
  ready_.notify_all();
  for (std::thread& worker : workers_) {
    worker.join();
  }
}

void ThreadPool::Loop() {
  uint64_t seen = 0;
  while (true) {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this, &seen] { return stop_ || generation_ != seen; });
    if (stop_) {
      return;
    }
    seen = generation_;
    while (next_task_ < task_count_) {
      const size_t task = next_task_++;
      const std::function<void(size_t)>* body = body_;
      lock.unlock();
      try {
        (*body)(task);
      } catch (...) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!failure_) {
          failure_ = std::current_exception();
        }
      }
      lock.lock();
      if (--pending_ == 0) {
        done_.notify_all();
      }
    }
  }
}

void ThreadPool::Run(size_t tasks, const std::function<void(size_t)>& body) {
  if (tasks == 0) {
    return;
  }
  std::exception_ptr failure;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    body_ = &body;
    task_count_ = tasks;
    next_task_ = 0;
    pending_ = tasks;
    failure_ = nullptr;
    ++generation_;
    ready_.notify_all();
    while (next_task_ < task_count_) {
      const size_t task = next_task_++;
      lock.unlock();
      try {
        body(task);
      } catch (...) {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!failure_) {
          failure_ = std::current_exception();
        }
      }
      lock.lock();
      if (--pending_ == 0) {
        done_.notify_all();
      }
    }
    done_.wait(lock, [this] { return pending_ == 0; });
    failure = failure_;
    failure_ = nullptr;
    body_ = nullptr;
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

}
