#include "infra/ThreadPool.h"

#include <algorithm>
#include <stdexcept>

namespace agent {
namespace infra {

ThreadPool::ThreadPool(std::size_t numThreads) {
  if (numThreads == 0) {
    numThreads = std::thread::hardware_concurrency();
    if (numThreads == 0) numThreads = 4;  // safe fallback
    numThreads = std::min(numThreads, static_cast<std::size_t>(8));
  }
  numWorkers_ = numThreads;
  workers_.reserve(numThreads);
  for (std::size_t i = 0; i < numThreads; ++i) {
    workers_.emplace_back(&ThreadPool::WorkerLoop, this);
  }
}

ThreadPool::~ThreadPool() {
  Shutdown();
}

void ThreadPool::WorkerLoop() {
  while (true) {
    TaskItem task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] {
        return stopped_ || !tasks_.empty();
      });
      if (stopped_ && tasks_.empty()) return;
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }
    activeWorkers_.fetch_add(1, std::memory_order_relaxed);
    try {
      task.func();
    } catch (...) {
      // Swallow exceptions from tasks — they're captured by packaged_task.
    }
    activeWorkers_.fetch_sub(1, std::memory_order_relaxed);
  }
}

void ThreadPool::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

void ThreadPool::ShutdownNow() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    tasks_.clear();
  }
  cv_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
}

std::size_t ThreadPool::PendingTasks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return tasks_.size();
}

std::size_t ThreadPool::ActiveWorkers() const {
  return activeWorkers_.load(std::memory_order_relaxed);
}

std::size_t ThreadPool::TotalWorkers() const {
  return numWorkers_;
}

bool ThreadPool::IsStopped() const {
  return stopped_.load(std::memory_order_relaxed);
}

ThreadPool& ThreadPool::Global() {
  static ThreadPool instance;
  return instance;
}

}  // namespace infra
}  // namespace agent
