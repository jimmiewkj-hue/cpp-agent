#include "core/AppStateStore.h"

#include <algorithm>
#include <chrono>

namespace agent {
namespace state {

// --- Task state ---

void AppStateStore::RegisterTask(const std::string& taskId,
                                  const std::string& description) {
  TaskState& ts = tasks_[taskId];
  ts.taskId = taskId;
  ts.description = description;
  ts.status = TaskStatus::Idle;
  ts.startedAtMs = NowMs();
  ts.lastActiveAtMs = ts.startedAtMs;
}

void AppStateStore::UpdateTaskStatus(const std::string& taskId,
                                      TaskStatus status,
                                      const std::string& reason) {
  auto it = tasks_.find(taskId);
  if (it == tasks_.end()) return;
  it->second.status = status;
  it->second.lastActiveAtMs = NowMs();
  if (!reason.empty()) it->second.terminalReason = reason;
  int statusInt = static_cast<int>(status);
  if (statusInt == static_cast<int>(TaskStatus::Completed) ||
      statusInt == static_cast<int>(TaskStatus::Failed) ||
      statusInt == static_cast<int>(TaskStatus::Killed)) {
    it->second.completedAtMs = NowMs();
  }
}

void AppStateStore::UpdateTaskActivity(const std::string& taskId) {
  auto it = tasks_.find(taskId);
  if (it == tasks_.end()) return;
  it->second.lastActiveAtMs = NowMs();
}

void AppStateStore::CompleteTask(const std::string& taskId,
                                  int outputTokens,
                                  const std::string& reason) {
  auto it = tasks_.find(taskId);
  if (it == tasks_.end()) return;
  it->second.status = TaskStatus::Completed;
  it->second.completedAtMs = NowMs();
  it->second.outputTokens = outputTokens;
  if (!reason.empty()) it->second.terminalReason = reason;
}

const TaskState* AppStateStore::GetTask(const std::string& taskId) const {
  auto it = tasks_.find(taskId);
  if (it == tasks_.end()) return nullptr;
  return &it->second;
}

std::vector<TaskState> AppStateStore::ListTasks(TaskStatus filter) const {
  std::vector<TaskState> result;
  int filterInt = static_cast<int>(filter);
  for (const auto& pair : tasks_) {
    const TaskState& ts = pair.second;
    if (filterInt == static_cast<int>(TaskStatus::All) ||
        static_cast<int>(ts.status) == filterInt) {
      result.push_back(ts);
    }
  }
  return result;
}

int AppStateStore::ActiveTaskCount() const {
  int count = 0;
  for (const auto& pair : tasks_) {
    if (static_cast<int>(pair.second.status) ==
        static_cast<int>(TaskStatus::Running)) {
      ++count;
    }
  }
  return count;
}

bool AppStateStore::HasRunningTasks() const {
  return ActiveTaskCount() > 0;
}

// --- Completion boundaries ---

void AppStateStore::RecordCompletionBoundary(const CompletionBoundary& boundary) {
  boundaries_.push_back(boundary);
  // Keep last 100 boundaries max
  if (boundaries_.size() > 100) {
    boundaries_.erase(boundaries_.begin());
  }
}

const CompletionBoundary* AppStateStore::LastBoundary() const {
  if (boundaries_.empty()) return nullptr;
  return &boundaries_.back();
}

std::vector<CompletionBoundary> AppStateStore::RecentBoundaries(int count) const {
  std::vector<CompletionBoundary> result;
  int start = std::max(0, static_cast<int>(boundaries_.size()) - count);
  for (int i = start; i < static_cast<int>(boundaries_.size()); ++i) {
    result.push_back(boundaries_[i]);
  }
  return result;
}

void AppStateStore::ClearBoundaries() {
  boundaries_.clear();
}

// --- Compact tracking ---

void AppStateStore::RecordCompaction(const std::string& type,
                                      int messages, int tokens) {
  ++compactTracking_.totalCompactions;
  compactTracking_.messagesCompacted += messages;
  compactTracking_.tokensCompacted += tokens;
  compactTracking_.lastCompactAtMs = NowMs();

  if (type == "session_memory") ++compactTracking_.sessionMemoryCompactions;
  else if (type == "reactive") ++compactTracking_.reactiveCompactions;
  else if (type == "time_based") ++compactTracking_.timeBasedMicrocompacts;
}

void AppStateStore::ResetCompactTracking() {
  compactTracking_ = CompactTrackingState{};
}

// --- Memory state ---

void AppStateStore::RecordMemoryExtraction(int count) {
  memoryState_.totalMemories += count;
  memoryState_.activeMemories += count;
  memoryState_.lastExtractionAtMs = NowMs();
}

void AppStateStore::RecordMemoryConsolidation() {
  ++memoryState_.consolidatedCount;
  memoryState_.lastConsolidationAtMs = NowMs();
}

void AppStateStore::MarkSessionMemoryInitialized() {
  memoryState_.sessionMemoryInitialized = true;
}

bool AppStateStore::IsSessionMemoryInitialized() const {
  return memoryState_.sessionMemoryInitialized;
}

// --- Session metadata ---

void AppStateStore::SetSessionMetadata(const std::string& key,
                                        const std::string& value) {
  sessionMetadata_[key] = value;
}

std::string AppStateStore::GetSessionMetadata(const std::string& key) const {
  auto it = sessionMetadata_.find(key);
  if (it == sessionMetadata_.end()) return std::string();
  return it->second;
}

bool AppStateStore::HasSessionMetadata(const std::string& key) const {
  return sessionMetadata_.find(key) != sessionMetadata_.end();
}

// --- Private ---

long long AppStateStore::NowMs() const {
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  return ms.count();
}

}  // namespace state
}  // namespace agent
