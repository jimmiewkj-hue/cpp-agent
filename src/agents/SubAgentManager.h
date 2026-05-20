#pragma once

#include "core/AgentTypes.h"

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {
namespace memory { class MemoryIndex; }
namespace agents {

struct SubAgentTask {
  std::string prompt;
  std::string description;
  std::string subagentType;
  std::string model;
  int priority = 50;
  int requiredExecutorSlots = 1;
  bool runInBackground = false;
  std::string isolation;
  std::string cwd;
  std::string parentCwd;
  std::string worktreeCwd;
  std::string name;
  std::string teamName;
  std::string mode;
};

struct SubAgentTaskCheckpoint {
  std::string checkpointId;
  std::string resumeCursor;
  long long savedAtUnixMs = 0;
  bool resumable = false;
};

enum class SubAgentTaskState {
  Pending,
  Running,
  Completed,
  Failed,
  Cancelled
};

enum class SubAgentExecutorState {
  Idle,
  Busy,
  Recovering,
  Offline
};

struct SubAgentExecutorSlot {
  std::string executorId;
  std::string hostName;
  int maxParallelTasks = 1;
  int runningTasks = 0;
  int weight = 1;
  bool healthy = true;
  bool supportsCheckpointResume = true;
  SubAgentExecutorState state = SubAgentExecutorState::Idle;
  long long lastHeartbeatUnixMs = 0;
  std::string lastError;
};

struct SubAgentTaskLifecycle {
  std::string taskId;
  SubAgentTask task;
  std::string directive;
  std::string worktreeNotice;
  std::string placeholderResult;
  std::string summary;
  SubAgentTaskState state = SubAgentTaskState::Pending;
  long long createdAtUnixMs = 0;
  long long updatedAtUnixMs = 0;
  std::string assignedExecutorId;
  std::string lastFailureReason;
  int attemptCount = 0;
  SubAgentTaskCheckpoint checkpoint;
};

struct SubAgentTaskSummary {
  int pending = 0;
  int running = 0;
  int completed = 0;
  int failed = 0;
  int cancelled = 0;
};

class SubAgentManager {
 public:
  static constexpr const char* kForkPlaceholderResult =
      u8"Fork started — processing in background";

  SubAgentManager();
  ~SubAgentManager();

  std::vector<core::Message> BuildForkedMessages(
      const std::string& directive,
      const core::Message& assistantMessage,
      const std::string& parentCwd = std::string(),
      const std::string& worktreeCwd = std::string(),
      const std::string& renderedSystemPrompt = std::string()) const;

  std::vector<std::string> ApplyExactToolWhitelist(
      const std::vector<std::string>& parentTools,
      const std::vector<std::string>& allowedTools) const;

  std::string BuildWorktreeNotice(
      const std::string& parentCwd,
      const std::string& worktreeCwd) const;
  std::string StartSubTask(const SubAgentTask& task);
  bool UpdateTaskState(
      const std::string& taskId,
      SubAgentTaskState state,
      const std::string& summary = std::string());
  bool TryGetTask(
      const std::string& taskId,
      SubAgentTaskLifecycle* lifecycle) const;
  std::vector<SubAgentTaskLifecycle> ListTasks() const;
  void SetExecutors(const std::vector<SubAgentExecutorSlot>& executors);
  void UpsertExecutor(const SubAgentExecutorSlot& executor);
  std::vector<SubAgentExecutorSlot> ListExecutors() const;
  void SetWorkerExecutablePath(const std::string& workerExecutablePath);
  void SetWorkerRuntimeRoot(const std::string& runtimeRoot);
  bool SaveCheckpoint(
      const std::string& taskId,
      const SubAgentTaskCheckpoint& checkpoint);
  bool RecordExecutorFailure(
      const std::string& executorId,
      const std::string& error);
  void RestoreTasksForRecovery(
      const std::vector<SubAgentTaskLifecycle>& tasks,
      const std::vector<SubAgentExecutorSlot>& executors =
          std::vector<SubAgentExecutorSlot>());
  SubAgentTaskSummary SummarizeTasks() const;
  bool IsForkCandidate(const core::Message& assistantMessage) const;
  bool IsInForkChild(const std::vector<core::Message>& messages) const;

  bool CreateWorktree(const std::string& taskId,
                      const std::string& baseBranch = "HEAD");
  bool HasWorktreeChanges(const std::string& worktreePath) const;
  bool RemoveWorktree(const std::string& worktreePath, bool force = true);
  std::string GetWorktreePath(const std::string& taskId) const;

  bool SendWorkerMessage(const std::string& taskId, const std::string& message);
  bool StopTask(const std::string& taskId);

  void SetMemoryIndex(memory::MemoryIndex* memoryIndex);
  void ExecuteAutoDream();

  std::string Cwd() const;

  struct BuiltInAgentDef {
    std::string name;
    std::string description;
    int maxTurns = 200;
    std::string model;
    std::string permissionMode;
    std::vector<std::string> allowedTools;
    std::string systemPromptHook;
  };
  static std::vector<BuiltInAgentDef> GetBuiltInAgentDefinitions();

  struct AgentToolProgress {
    std::string taskId;
    std::string phase;
    std::string output;
    bool done = false;
    int exitCode = 0;
  };
  std::string RunAsyncAgentLifecycle(
      const std::string& prompt,
      const std::string& description,
      const std::string& subagentType,
      bool runInBackground,
      const std::string& isolation,
      const std::vector<std::string>& allowedTools);

 private:
  struct WorkerRuntime;

  void EnsureRuntimeDefaultsLocked();
  void RebuildWorkerAssignmentsLocked();
  void MonitorLoop();
  void RefreshWorkerStatesLocked();
  void SpawnWorkerForTaskLocked(std::size_t taskIndex, std::size_t executorIndex);
  void RequestWorkerStopLocked(
      const std::string& taskId,
      bool preemptive,
      const std::string& reason);
  int FindPreemptibleTaskLocked(const SubAgentTaskLifecycle& task) const;
  void NormalizeExecutorLoadLocked();
  void SchedulePendingTasksLocked();
  int FindBestExecutorLocked(const SubAgentTaskLifecycle& task) const;

  mutable std::mutex mutex_;
  std::condition_variable monitorCv_;
  bool stopMonitor_ = false;
  std::thread monitorThread_;
  int nextTaskId_ = 1;
  std::vector<SubAgentTaskLifecycle> tasks_;
  std::vector<SubAgentExecutorSlot> executors_;
  std::map<std::string, WorkerRuntime> workers_;
  std::string workerExecutablePath_;
  std::string runtimeRoot_;
};

}  // namespace agents
}  // namespace agent
