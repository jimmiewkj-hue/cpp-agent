#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {
namespace infra {

struct StabilityMetrics {
  int totalTurns = 0;
  int failedTurns = 0;
  int recoveredTurns = 0;
  int oomCount = 0;
  int deadlockCount = 0;
  int crashCount = 0;
  int pendingSubTasks = 0;
  int runningSubTasks = 0;
  int failedSubTasks = 0;
  double averageTurnTimeMs = 0.0;
  double lastHeartbeatAgeMs = 0.0;
  bool healthy = true;
};

struct TaskStateMetrics {
  int pending = 0;
  int running = 0;
  int failed = 0;
};

struct StabilityConfig {
  int heartbeatIntervalMs = 5000;
  int heartbeatTimeoutMs = 30000;
  int maxConsecutiveFailures = 10;
  int maxMemoryBytes = 1024 * 1024 * 1024;
  int maxTurnsPerSession = 2000;
  int snapshotIntervalTurns = 50;
  int deadlockThresholdTurns = 100;
  bool autoRecover = true;
};

class StabilityWatchdog {
 public:
  using CrashRecoveryCallback = std::function<bool()>;
  using SnapshotCallback = std::function<void()>;
  using ResourceCheckCallback = std::function<bool()>;
  using TaskStateCallback = std::function<TaskStateMetrics()>;

  explicit StabilityWatchdog(const StabilityConfig& config);

  void Start();
  void Stop();
  bool IsHealthy() const;

  void SetCrashRecoveryCallback(CrashRecoveryCallback callback);
  void SetSnapshotCallback(SnapshotCallback callback);
  void SetResourceCheckCallback(ResourceCheckCallback callback);
  void SetTaskStateCallback(TaskStateCallback callback);

  void Heartbeat();
  void SignalTurnComplete(bool success);
  void SignalRecovery();
  void SignalOom();
  void SignalDeadlock();

  StabilityMetrics metrics() const;
  StabilityConfig& config();

 private:
  void MonitorLoop();
  bool PerformHealthCheck();
  bool PerformRecovery();

  StabilityConfig config_;
  StabilityMetrics metrics_;
  mutable std::mutex mutex_;

  std::atomic<bool> running_{false};
  std::atomic<int64_t> lastHeartbeat_{0};
  std::atomic<int> consecutiveFailures_{0};
  std::thread monitorThread_;

  CrashRecoveryCallback crashRecoveryCallback_;
  SnapshotCallback snapshotCallback_;
  ResourceCheckCallback resourceCheckCallback_;
  TaskStateCallback taskStateCallback_;
};

}  // namespace infra
}  // namespace agent
