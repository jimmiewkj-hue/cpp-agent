#pragma once

// Fixed-size thread pool for parallel I/O operations in cpp-agent.
// Used to offload file reads, tool execution, session persistence, and
// other I/O-bound work to worker threads, reducing LLM wait time.
//
// Design aligned with local-ace's approach of fire-and-forget transcript
// writes and parallel tool execution for concurrent-safe batches.
//
// Usage:
//   ThreadPool pool(4);
//   auto future = pool.Submit([]() { return ReadFile("foo.txt"); });
//   std::string content = future.get();
//
//   // Parallel batch
//   auto futures = pool.SubmitAll({
//     []() { return ReadFile("a.txt"); },
//     []() { return ReadFile("b.txt"); },
//   });
//   for (auto& f : futures) results.push_back(f.get());

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {
namespace infra {

enum class TaskPriority {
  LOW = 0,
  NORMAL = 1,
  HIGH = 2,    // model-call-adjacent work
};

class ThreadPool {
 public:
  // Create a pool with `numThreads` workers.
  // Default: hardware_concurrency() capped at 8.
  explicit ThreadPool(std::size_t numThreads = 0);

  ~ThreadPool();

  // Non-copyable, non-movable.
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Submit a single task. Returns a future for the result.
  template <typename F, typename R = std::invoke_result_t<F>>
  std::future<R> Submit(F&& func, TaskPriority priority = TaskPriority::NORMAL) {
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(func));
    auto future = task->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopped_) {
        // If pool is stopped, return a future with an exception
        auto promise = std::make_shared<std::promise<R>>();
        promise->set_exception(
            std::make_exception_ptr(std::runtime_error("ThreadPool is stopped")));
        return promise->get_future();
      }
      tasks_.push_back({priority, [task]() { (*task)(); }});
      // Sort so HIGH priority items are at the front
      if (priority == TaskPriority::HIGH) {
        // Move high priority items to the front (stable relative to other high)
        std::stable_sort(tasks_.begin(), tasks_.end(),
                         [](const TaskItem& a, const TaskItem& b) {
                           return a.priority > b.priority;
                         });
      }
    }
    cv_.notify_one();
    return future;
  }

  // Submit multiple tasks in parallel. Returns futures for each result.
  template <typename F, typename R = std::invoke_result_t<F>>
  std::vector<std::future<R>> SubmitAll(std::vector<F> funcs,
                                        TaskPriority priority = TaskPriority::NORMAL) {
    std::vector<std::future<R>> futures;
    futures.reserve(funcs.size());
    for (auto& func : funcs) {
      futures.push_back(Submit(std::move(func), priority));
    }
    return futures;
  }

  // Wait for all futures in a vector. Collects results.
  template <typename T>
  static std::vector<T> WaitAll(std::vector<std::future<T>>& futures) {
    std::vector<T> results;
    results.reserve(futures.size());
    for (auto& f : futures) {
      results.push_back(f.get());
    }
    return results;
  }

  // Graceful shutdown: finish all pending tasks, then join workers.
  void Shutdown();

  // Immediate shutdown: discard pending tasks, join workers.
  void ShutdownNow();

  // Get pool statistics.
  std::size_t PendingTasks() const;
  std::size_t ActiveWorkers() const;
  std::size_t TotalWorkers() const;
  bool IsStopped() const;

  // Global shared pool instance (lazy-initialized).
  static ThreadPool& Global();

 private:
  struct TaskItem {
    TaskPriority priority = TaskPriority::NORMAL;
    std::function<void()> func;
  };

  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::deque<TaskItem> tasks_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::atomic<bool> stopped_{false};
  std::atomic<std::size_t> activeWorkers_{0};
  std::size_t numWorkers_ = 0;
};

}  // namespace infra
}  // namespace agent
