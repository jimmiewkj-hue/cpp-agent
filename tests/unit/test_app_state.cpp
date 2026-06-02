// P0-03: Comprehensive tests for AppStateStore (aligned with local-ace).
// Covers: task lifecycle, completion boundaries, compact tracking,
// memory state, session metadata, edge cases, real-world scenarios.

#include <cassert>
#include <iostream>
#include <string>
#include <thread>

#define TEST(name) void test_##name()
#define RUN(name) do { std::cout << "  " << #name << "... "; test_##name(); std::cout << "PASSED" << std::endl; } while(0)
#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED: " << #cond << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_EQ(a, b) do { if ((a) != (b)) { std::cerr << "FAILED: " << #a << " == " << #b << " (" << (a) << " != " << (b) << ") at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)
#define CHECK_NE(a, b) do { if ((a) == (b)) { std::cerr << "FAILED: " << #a << " != " << #b << " at " << __FILE__ << ":" << __LINE__ << std::endl; std::abort(); } } while(0)

#include "core/AppStateStore.h"

using namespace agent::state;

// ============================================================================
// Task lifecycle tests
// ============================================================================

TEST(register_task_creates_idle_task) {
  AppStateStore store;
  store.RegisterTask("task-1", "Build character graph");
  
  const TaskState* ts = store.GetTask("task-1");
  CHECK(ts != nullptr);
  CHECK_EQ(ts->taskId, "task-1");
  CHECK_EQ(ts->description, "Build character graph");
  CHECK(static_cast<int>(ts->status) == static_cast<int>(TaskStatus::Idle));
  CHECK(ts->startedAtMs > 0);
}

TEST(get_nonexistent_task_returns_null) {
  AppStateStore store;
  const TaskState* ts = store.GetTask("nonexistent");
  CHECK(ts == nullptr);
}

TEST(update_task_status) {
  AppStateStore store;
  store.RegisterTask("task-1", "Test task");
  store.UpdateTaskStatus("task-1", TaskStatus::Running);
  
  const TaskState* ts = store.GetTask("task-1");
  CHECK(static_cast<int>(ts->status) == static_cast<int>(TaskStatus::Running));
}

TEST(complete_task_sets_status_and_tokens) {
  AppStateStore store;
  store.RegisterTask("task-1", "Test task");
  store.CompleteTask("task-1", 5000, "Done successfully");
  
  const TaskState* ts = store.GetTask("task-1");
  CHECK(static_cast<int>(ts->status) == static_cast<int>(TaskStatus::Completed));
  CHECK_EQ(ts->outputTokens, 5000);
  CHECK_EQ(ts->terminalReason, "Done successfully");
  CHECK(ts->completedAtMs > 0);
}

TEST(failed_task_sets_status) {
  AppStateStore store;
  store.RegisterTask("task-1", "Test task");
  store.UpdateTaskStatus("task-1", TaskStatus::Failed, "Network error");
  
  const TaskState* ts = store.GetTask("task-1");
  CHECK(static_cast<int>(ts->status) == static_cast<int>(TaskStatus::Failed));
  CHECK_EQ(ts->terminalReason, "Network error");
}

TEST(list_tasks_all) {
  AppStateStore store;
  store.RegisterTask("a", "Task A");
  store.RegisterTask("b", "Task B");
  store.CompleteTask("a", 100);
  
  auto all = store.ListTasks(TaskStatus::All);
  CHECK_EQ(static_cast<int>(all.size()), 2);
}

TEST(list_tasks_by_status) {
  AppStateStore store;
  store.RegisterTask("a", "Task A");
  store.RegisterTask("b", "Task B");
  store.UpdateTaskStatus("a", TaskStatus::Running);
  
  auto running = store.ListTasks(TaskStatus::Running);
  CHECK_EQ(static_cast<int>(running.size()), 1);
  CHECK_EQ(running[0].taskId, "a");
  
  auto idle = store.ListTasks(TaskStatus::Idle);
  CHECK_EQ(static_cast<int>(idle.size()), 1);
  CHECK_EQ(idle[0].taskId, "b");
}

TEST(active_task_count) {
  AppStateStore store;
  CHECK_EQ(store.ActiveTaskCount(), 0);
  
  store.RegisterTask("a", "Task A");
  store.UpdateTaskStatus("a", TaskStatus::Running);
  CHECK_EQ(store.ActiveTaskCount(), 1);
  
  store.RegisterTask("b", "Task B");
  store.UpdateTaskStatus("b", TaskStatus::Running);
  CHECK_EQ(store.ActiveTaskCount(), 2);
  
  store.CompleteTask("a", 100);
  CHECK_EQ(store.ActiveTaskCount(), 1);
}

TEST(has_running_tasks) {
  AppStateStore store;
  CHECK(!store.HasRunningTasks());
  
  store.RegisterTask("a", "Task A");
  store.UpdateTaskStatus("a", TaskStatus::Running);
  CHECK(store.HasRunningTasks());
  
  store.CompleteTask("a", 100);
  CHECK(!store.HasRunningTasks());
}

TEST(update_task_activity) {
  AppStateStore store;
  store.RegisterTask("task-1", "Test");
  
  const TaskState* ts1 = store.GetTask("task-1");
  long long before = ts1->lastActiveAtMs;
  
  // Wait briefly
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  store.UpdateTaskActivity("task-1");
  
  const TaskState* ts2 = store.GetTask("task-1");
  CHECK(ts2->lastActiveAtMs > before);
}

// ============================================================================
// Completion boundary tests
// ============================================================================

TEST(record_completion_boundary) {
  AppStateStore store;
  CompletionBoundary cb;
  cb.type = CompletionBoundary::Type::Complete;
  cb.completedAtMs = 1000;
  cb.outputTokens = 500;
  
  store.RecordCompletionBoundary(cb);
  
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last != nullptr);
  CHECK_EQ(last->outputTokens, 500);
}

TEST(recent_boundaries_limit) {
  AppStateStore store;
  for (int i = 0; i < 10; ++i) {
    CompletionBoundary cb;
    cb.type = CompletionBoundary::Type::Bash;
    cb.command = "cmd-" + std::to_string(i);
    cb.completedAtMs = 1000 + i * 100;
    store.RecordCompletionBoundary(cb);
  }
  
  auto recent = store.RecentBoundaries(5);
  CHECK_EQ(static_cast<int>(recent.size()), 5);
  // Most recent should be last
  CHECK(recent.back().command == "cmd-9");
}

TEST(bash_boundary_stores_command) {
  AppStateStore store;
  CompletionBoundary cb;
  cb.type = CompletionBoundary::Type::Bash;
  cb.command = "pip list";
  cb.completedAtMs = 2000;
  
  store.RecordCompletionBoundary(cb);
  
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last != nullptr);
  CHECK_EQ(last->command, "pip list");
}

TEST(edit_boundary_stores_file_path) {
  AppStateStore store;
  CompletionBoundary cb;
  cb.type = CompletionBoundary::Type::Edit;
  cb.toolName = "Write";
  cb.filePath = "/path/to/file.py";
  
  store.RecordCompletionBoundary(cb);
  
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last != nullptr);
  CHECK_EQ(last->toolName, "Write");
  CHECK_EQ(last->filePath, "/path/to/file.py");
}

TEST(denied_tool_boundary_stores_detail) {
  AppStateStore store;
  CompletionBoundary cb;
  cb.type = CompletionBoundary::Type::DeniedTool;
  cb.toolName = "DeleteFile";
  cb.detail = "Permission denied: workspace boundary";
  
  store.RecordCompletionBoundary(cb);
  
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last != nullptr);
  CHECK_EQ(last->toolName, "DeleteFile");
  CHECK(last->detail.find("Permission denied") != std::string::npos);
}

TEST(clear_boundaries) {
  AppStateStore store;
  CompletionBoundary cb;
  store.RecordCompletionBoundary(cb);
  store.RecordCompletionBoundary(cb);
  
  store.ClearBoundaries();
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last == nullptr);
}

// ============================================================================
// Compact tracking tests
// ============================================================================

TEST(record_compaction_increments_counters) {
  AppStateStore store;
  
  store.RecordCompaction("session_memory", 10, 5000);
  const auto& cs = store.CompactState();
  CHECK_EQ(cs.totalCompactions, 1);
  CHECK_EQ(cs.sessionMemoryCompactions, 1);
  CHECK_EQ(cs.messagesCompacted, 10);
  CHECK_EQ(cs.tokensCompacted, 5000);
  
  store.RecordCompaction("reactive", 5, 2000);
  CHECK_EQ(cs.totalCompactions, 2);
  CHECK_EQ(cs.reactiveCompactions, 1);
  CHECK_EQ(cs.messagesCompacted, 15);
  CHECK_EQ(cs.tokensCompacted, 7000);
}

TEST(record_time_based_compaction) {
  AppStateStore store;
  store.RecordCompaction("time_based", 3, 1000);
  
  const auto& cs = store.CompactState();
  CHECK_EQ(cs.timeBasedMicrocompacts, 1);
}

TEST(reset_compact_tracking) {
  AppStateStore store;
  store.RecordCompaction("session_memory", 10, 5000);
  store.ResetCompactTracking();
  
  const auto& cs = store.CompactState();
  CHECK_EQ(cs.totalCompactions, 0);
  CHECK_EQ(cs.messagesCompacted, 0);
}

// ============================================================================
// Memory state tests
// ============================================================================

TEST(record_memory_extraction) {
  AppStateStore store;
  store.RecordMemoryExtraction(5);
  
  const auto& ms = store.GetMemoryState();
  CHECK_EQ(ms.totalMemories, 5);
  CHECK_EQ(ms.activeMemories, 5);
  CHECK(ms.lastExtractionAtMs > 0);
}

TEST(record_memory_consolidation) {
  AppStateStore store;
  store.RecordMemoryConsolidation();
  
  const auto& ms = store.GetMemoryState();
  CHECK_EQ(ms.consolidatedCount, 1);
  CHECK(ms.lastConsolidationAtMs > 0);
}

TEST(session_memory_initialized_flag) {
  AppStateStore store;
  CHECK(!store.IsSessionMemoryInitialized());
  
  store.MarkSessionMemoryInitialized();
  CHECK(store.IsSessionMemoryInitialized());
}

// ============================================================================
// Session metadata tests
// ============================================================================

TEST(session_metadata_crud) {
  AppStateStore store;
  CHECK(!store.HasSessionMetadata("key1"));
  
  store.SetSessionMetadata("key1", "value1");
  CHECK(store.HasSessionMetadata("key1"));
  CHECK_EQ(store.GetSessionMetadata("key1"), "value1");
}

TEST(session_metadata_nonexistent_returns_empty) {
  AppStateStore store;
  CHECK(store.GetSessionMetadata("nonexistent").empty());
}

TEST(session_metadata_overwrite) {
  AppStateStore store;
  store.SetSessionMetadata("key", "old");
  store.SetSessionMetadata("key", "new");
  CHECK_EQ(store.GetSessionMetadata("key"), "new");
}

// ============================================================================
// Real-world scenario: full task lifecycle with compaction tracking
// ============================================================================

TEST(real_world_full_session_lifecycle) {
  AppStateStore store;
  
  // Session starts, register tasks
  store.RegisterTask("main", "Build character relationship graph");
  store.RegisterTask("sub-1", "Extract character names");
  store.RegisterTask("sub-2", "Build relationship edges");
  
  // Main task starts running
  store.UpdateTaskStatus("main", TaskStatus::Running);
  CHECK(store.HasRunningTasks());
  CHECK_EQ(store.ActiveTaskCount(), 1);
  
  // Sub-tasks start
  store.UpdateTaskStatus("sub-1", TaskStatus::Running);
  store.UpdateTaskStatus("sub-2", TaskStatus::Running);
  CHECK_EQ(store.ActiveTaskCount(), 3);
  
  // Memory extraction happens
  store.RecordMemoryExtraction(10);
  store.MarkSessionMemoryInitialized();
  
  // Compaction happens during processing
  store.RecordCompaction("session_memory", 20, 8000);
  
  // Sub-task 1 completes
  store.CompleteTask("sub-1", 3000, "Character names extracted");
  
  // Bash command boundary recorded
  CompletionBoundary bash;
  bash.type = CompletionBoundary::Type::Bash;
  bash.command = "python analyze.py";
  bash.completedAtMs = 5000;
  store.RecordCompletionBoundary(bash);
  
  // Sub-task 2 fails
  store.UpdateTaskStatus("sub-2", TaskStatus::Failed, "Memory exceeded");
  CHECK_EQ(store.ActiveTaskCount(), 1);  // Only main still running
  
  // Verify state consistency
  const auto& ms = store.GetMemoryState();
  CHECK(ms.sessionMemoryInitialized);
  CHECK_EQ(ms.totalMemories, 10);
  
  const auto& cs = store.CompactState();
  CHECK_EQ(cs.totalCompactions, 1);
  CHECK_EQ(cs.sessionMemoryCompactions, 1);
  
  const CompletionBoundary* last = store.LastBoundary();
  CHECK(last != nullptr);
  CHECK_EQ(last->command, "python analyze.py");
  
  // Complete main task
  store.CompleteTask("main", 10000, "Graph generation complete");
  CHECK(!store.HasRunningTasks());
}

int main() {
  std::cout << "=== AppStateStore Tests ===" << std::endl;
  
  std::cout << "[Task Lifecycle]" << std::endl;
  RUN(register_task_creates_idle_task);
  RUN(get_nonexistent_task_returns_null);
  RUN(update_task_status);
  RUN(complete_task_sets_status_and_tokens);
  RUN(failed_task_sets_status);
  RUN(list_tasks_all);
  RUN(list_tasks_by_status);
  RUN(active_task_count);
  RUN(has_running_tasks);
  RUN(update_task_activity);
  
  std::cout << "[Completion Boundaries]" << std::endl;
  RUN(record_completion_boundary);
  RUN(recent_boundaries_limit);
  RUN(bash_boundary_stores_command);
  RUN(edit_boundary_stores_file_path);
  RUN(denied_tool_boundary_stores_detail);
  RUN(clear_boundaries);
  
  std::cout << "[Compact Tracking]" << std::endl;
  RUN(record_compaction_increments_counters);
  RUN(record_time_based_compaction);
  RUN(reset_compact_tracking);
  
  std::cout << "[Memory State]" << std::endl;
  RUN(record_memory_extraction);
  RUN(record_memory_consolidation);
  RUN(session_memory_initialized_flag);
  
  std::cout << "[Session Metadata]" << std::endl;
  RUN(session_metadata_crud);
  RUN(session_metadata_nonexistent_returns_empty);
  RUN(session_metadata_overwrite);
  
  std::cout << "[Real-World Scenario]" << std::endl;
  RUN(real_world_full_session_lifecycle);
  
  std::cout << "\nAll AppStateStore tests PASSED" << std::endl;
  return 0;
}
