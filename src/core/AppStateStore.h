#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace agent {
namespace state {

// P0-03: AppStateStore aligned with local-ace AppStateStore.
// Central state management for task states, memory state, compact tracking,
// and session-level metadata.

enum class TaskStatus {
  Idle,
  Running,
  Completed,
  Killed,
  Failed,
  Blocked,
  All  // Sentinel: matches any status
};

struct TaskState {
  std::string taskId;
  std::string description;
  TaskStatus status = TaskStatus::Idle;
  int turnCount = 0;
  long long startedAtMs = 0;
  long long completedAtMs = 0;
  long long lastActiveAtMs = 0;
  int outputTokens = 0;
  std::string terminalReason;
};

// Completion boundary tracking (aligned with local-ace CompletionBoundary)
struct CompletionBoundary {
  enum class Type { Complete, Bash, Edit, DeniedTool };
  Type type = Type::Complete;
  long long completedAtMs = 0;
  int outputTokens = 0;
  std::string command;    // For Bash type
  std::string toolName;   // For Edit/DeniedTool types
  std::string filePath;   // For Edit type
  std::string detail;     // For DeniedTool type
};

// Compact tracking state
struct CompactTrackingState {
  int totalCompactions = 0;
  int sessionMemoryCompactions = 0;
  int reactiveCompactions = 0;
  int timeBasedMicrocompacts = 0;
  long long lastCompactAtMs = 0;
  int messagesCompacted = 0;
  int tokensCompacted = 0;
};

// Memory state tracking
struct MemoryState {
  int totalMemories = 0;
  int activeMemories = 0;
  int consolidatedCount = 0;
  long long lastConsolidationAtMs = 0;
  long long lastExtractionAtMs = 0;
  bool sessionMemoryInitialized = false;
};

class AppStateStore {
 public:
  AppStateStore() = default;

  // --- Task state ---
  void RegisterTask(const std::string& taskId, const std::string& description);
  void UpdateTaskStatus(const std::string& taskId, TaskStatus status,
                        const std::string& reason = "");
  void UpdateTaskActivity(const std::string& taskId);
  void CompleteTask(const std::string& taskId, int outputTokens = 0,
                    const std::string& reason = "");
  const TaskState* GetTask(const std::string& taskId) const;
  std::vector<TaskState> ListTasks(TaskStatus filter = TaskStatus::All) const;
  int ActiveTaskCount() const;
  bool HasRunningTasks() const;

  // --- Completion boundaries ---
  void RecordCompletionBoundary(const CompletionBoundary& boundary);
  const CompletionBoundary* LastBoundary() const;
  std::vector<CompletionBoundary> RecentBoundaries(int count = 10) const;
  void ClearBoundaries();

  // --- Compact tracking ---
  void RecordCompaction(const std::string& type, int messages, int tokens);
  const CompactTrackingState& CompactState() const { return compactTracking_; }
  void ResetCompactTracking();

  // --- Memory state ---
  void RecordMemoryExtraction(int count);
  void RecordMemoryConsolidation();
  void MarkSessionMemoryInitialized();
  const MemoryState& GetMemoryState() const { return memoryState_; }
  bool IsSessionMemoryInitialized() const;

  // --- Session metadata ---
  void SetSessionMetadata(const std::string& key, const std::string& value);
  std::string GetSessionMetadata(const std::string& key) const;
  bool HasSessionMetadata(const std::string& key) const;

 private:
  long long NowMs() const;

  std::map<std::string, TaskState> tasks_;
  std::vector<CompletionBoundary> boundaries_;
  CompactTrackingState compactTracking_;
  MemoryState memoryState_;
  std::map<std::string, std::string> sessionMetadata_;
};

}  // namespace state
}  // namespace agent
