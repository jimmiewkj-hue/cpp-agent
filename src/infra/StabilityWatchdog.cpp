#include "infra/StabilityWatchdog.h"

#include <chrono>
#include <iostream>

namespace agent {
namespace infra {

StabilityWatchdog::StabilityWatchdog(const StabilityConfig& config)
    : config_(config) {
  lastHeartbeat_.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

void StabilityWatchdog::Start() {
  if (running_.exchange(true)) return;
  monitorThread_ = std::thread(&StabilityWatchdog::MonitorLoop, this);
}

void StabilityWatchdog::Stop() {
  running_.store(false);
  if (monitorThread_.joinable()) {
    monitorThread_.join();
  }
}

bool StabilityWatchdog::IsHealthy() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_.healthy;
}

void StabilityWatchdog::SetCrashRecoveryCallback(
    CrashRecoveryCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  crashRecoveryCallback_ = std::move(callback);
}

void StabilityWatchdog::SetSnapshotCallback(SnapshotCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  snapshotCallback_ = std::move(callback);
}

void StabilityWatchdog::SetResourceCheckCallback(
    ResourceCheckCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  resourceCheckCallback_ = std::move(callback);
}

void StabilityWatchdog::SetTaskStateCallback(TaskStateCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  taskStateCallback_ = std::move(callback);
}

void StabilityWatchdog::Heartbeat() {
  lastHeartbeat_.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  consecutiveFailures_.store(0);
}

void StabilityWatchdog::SignalTurnComplete(bool success) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.totalTurns;
  }
  if (!success) {
    consecutiveFailures_.fetch_add(1);
  }
}

void StabilityWatchdog::SignalRecovery() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metrics_.recoveredTurns;
}

void StabilityWatchdog::SignalOom() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metrics_.oomCount;
}

void StabilityWatchdog::SignalDeadlock() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++metrics_.deadlockCount;
}

StabilityMetrics StabilityWatchdog::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
}

StabilityConfig& StabilityWatchdog::config() {
  return config_;
}

void StabilityWatchdog::MonitorLoop() {
  while (running_.load()) {
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.heartbeatIntervalMs));

    if (!PerformHealthCheck()) {
      if (config_.autoRecover) {
        PerformRecovery();
      }
    }
  }
}

bool StabilityWatchdog::PerformHealthCheck() {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const int64_t age = now - lastHeartbeat_.load();

  bool healthy = true;

  if (age > static_cast<int64_t>(config_.heartbeatTimeoutMs)) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.healthy = false;
    metrics_.lastHeartbeatAgeMs = static_cast<double>(age);
    healthy = false;
  }

  if (consecutiveFailures_.load() > config_.maxConsecutiveFailures) {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.healthy = false;
    healthy = false;
  }

  if (resourceCheckCallback_) {
    if (!resourceCheckCallback_()) {
      std::lock_guard<std::mutex> lock(mutex_);
      metrics_.healthy = false;
      healthy = false;
    }
  }

  if (taskStateCallback_) {
    const TaskStateMetrics taskMetrics = taskStateCallback_();
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.pendingSubTasks = taskMetrics.pending;
    metrics_.runningSubTasks = taskMetrics.running;
    metrics_.failedSubTasks = taskMetrics.failed;
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (healthy) {
      metrics_.healthy = true;
    }
  }

  return healthy;
}

bool StabilityWatchdog::PerformRecovery() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.crashCount;
  }

  if (snapshotCallback_) {
    snapshotCallback_();
  }

  if (crashRecoveryCallback_) {
    const bool recovered = crashRecoveryCallback_();
    if (recovered) {
      std::lock_guard<std::mutex> lock(mutex_);
      metrics_.healthy = true;
      ++metrics_.recoveredTurns;
      Heartbeat();
      return true;
    }
  }

  return false;
}

}  // namespace infra
}  // namespace agent
