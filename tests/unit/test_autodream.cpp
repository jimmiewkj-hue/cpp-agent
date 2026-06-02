#include "memory/AutoDream.h"
#include "memory/MemoryIndex.h"

#include <cassert>
#include <cstdio>
#include <iostream>
#include <string>
#include <windows.h>

namespace {

int failures = 0;
void Check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << "\n"; ++failures; }
}

std::string MakeTempDir() {
  char tmpl[MAX_PATH];
  GetTempPathA(MAX_PATH, tmpl);
  std::string dir = std::string(tmpl) + "cpp_agent_autodream_test_" + std::to_string(GetCurrentProcessId());
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
}

void RemoveDir(const std::string& dir) {
  std::string cmd = "rmdir /s /q \"" + dir + "\"";
  system(cmd.c_str());
}

// ============================================================================
// Test 1: Config defaults match local-ace DEFAULTS
// ============================================================================
void TestConfigDefaults() {
  agent::memory::AutoDreamConfig config;
  Check(config.minHours == 24, "minHours default should be 24");
  Check(config.minSessions == 5, "minSessions default should be 5");
  Check(config.scanThrottleMs == 10 * 60 * 1000, "scanThrottleMs default should be 10min");
}

// ============================================================================
// Test 2: Enable/Disable toggles state correctly
// ============================================================================
void TestEnableDisable() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  Check(engine.IsEnabled(), "Engine should be enabled by default");
  engine.Disable();
  Check(!engine.IsEnabled(), "Engine should be disabled after Disable()");
  engine.Enable();
  Check(engine.IsEnabled(), "Engine should be re-enabled after Enable()");
  RemoveDir(dir);
}

// ============================================================================
// Test 3: Configure does not change enabled state
// ============================================================================
void TestConfigure() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  agent::memory::AutoDreamConfig cfg;
  cfg.minHours = 48;
  cfg.minSessions = 10;
  engine.Configure(cfg);
  
  Check(engine.IsEnabled(), "Engine should still be enabled after Configure");
  engine.Disable();
  engine.Configure(cfg);
  Check(!engine.IsEnabled(), "Engine should still be disabled after Configure on disabled engine");
  RemoveDir(dir);
}

// ============================================================================
// Test 4: Time gate not passed immediately after construction
// ============================================================================
void TestTimeGateImmediate() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  // Immediately after construction, lastConsolidatedAtMs is set to NowUnixMs()
  // so time gate should NOT pass (needs minHours=24 to elapse)
  bool gate = engine.IsTimeGatePassed();
  Check(!gate, "Time gate should NOT pass immediately after construction");
  RemoveDir(dir);
}

// ============================================================================
// Test 5: IsGateOpen is false when disabled
// ============================================================================
void TestGateOpenDisabled() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  Check(engine.IsGateOpen(), "Gate should be open when enabled and fresh");
  
  engine.Disable();
  Check(!engine.IsGateOpen(), "Gate should be closed when disabled");
  RemoveDir(dir);
}

// ============================================================================
// Test 6: ShouldExecute returns false when disabled
// ============================================================================
void TestShouldExecuteDisabled() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  engine.Disable();
  
  bool should = engine.ShouldExecute();
  Check(!should, "ShouldExecute should return false when disabled");
  RemoveDir(dir);
}

// ============================================================================
// Test 7: ShouldExecute respects time gate
// ============================================================================
void TestShouldExecuteTimeGate() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  // Time gate is closed immediately after construction
  // So ShouldExecute should be false
  bool timeOpen = engine.IsTimeGatePassed();
  bool should = engine.ShouldExecute();
  
  if (!timeOpen) {
    Check(!should, "ShouldExecute should be false when time gate is closed");
  }
  
  // When disabled, ShouldExecute must be false regardless of time gate
  engine.Disable();
  Check(!engine.ShouldExecute(), "ShouldExecute must be false when disabled (time gate irrelevant)");
  RemoveDir(dir);
}

// ============================================================================
// Test 8: DreamTask lifecycle - register, get, complete, fail
// ============================================================================
void TestDreamTaskLifecycle() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  // Register
  agent::memory::DreamTaskState task;
  task.taskId = "task-001";
  task.status = agent::memory::DreamTaskStatus::Running;
  task.sessionsReviewing = 5;
  task.priorMtimeMs = 123456789;
  
  std::string id = engine.RegisterDreamTask(task);
  Check(!id.empty(), "RegisterDreamTask should return non-empty id");
  
  // Get
  const auto* found = engine.GetDreamTask(id);
  Check(found != nullptr, "GetDreamTask should find registered task");
  if (found) {
    Check(found->sessionsReviewing == 5, "Found task should preserve sessionsReviewing");
    Check(found->status == agent::memory::DreamTaskStatus::Running, "Found task should be Running");
  }
  
  // Get nonexistent
  Check(engine.GetDreamTask("ghost") == nullptr, "GetDreamTask should return nullptr for nonexistent task");
  
  // Complete
  engine.CompleteDreamTask(id);
  const auto* done = engine.GetDreamTask(id);
  Check(done != nullptr, "Completed task should still exist");
  if (done) {
    Check(done->status == agent::memory::DreamTaskStatus::Completed, "Task status should be Completed");
  }
  
  // Fail
  agent::memory::DreamTaskState task2;
  task2.taskId = "task-002";
  task2.status = agent::memory::DreamTaskStatus::Running;
  std::string id2 = engine.RegisterDreamTask(task2);
  engine.FailDreamTask(id2);
  const auto* failed = engine.GetDreamTask(id2);
  Check(failed != nullptr, "Failed task should still exist");
  if (failed) {
    Check(failed->status == agent::memory::DreamTaskStatus::Failed, "Task status should be Failed");
  }
  RemoveDir(dir);
}

// ============================================================================
// Test 9: Multiple concurrent task registration
// ============================================================================
void TestMultipleTasks() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  std::vector<std::string> ids;
  for (int i = 0; i < 5; ++i) {
    agent::memory::DreamTaskState t;
    t.taskId = "batch-" + std::to_string(i);
    t.status = agent::memory::DreamTaskStatus::Running;
    std::string id = engine.RegisterDreamTask(t);
    Check(!id.empty(), "Each registration should return non-empty id");
    ids.push_back(id);
  }
  
  // All should be retrievable
  for (const auto& id : ids) {
    Check(engine.GetDreamTask(id) != nullptr, "Each registered task should be retrievable");
  }
  RemoveDir(dir);
}

// ============================================================================
// Test 10: DreamTaskStatus enum values are stable
// ============================================================================
void TestDreamTaskStatusEnum() {
  using agent::memory::DreamTaskStatus;
  Check(static_cast<int>(DreamTaskStatus::Idle) == 0, "Idle should be 0");
  Check(static_cast<int>(DreamTaskStatus::Running) == 1, "Running should be 1");
  Check(static_cast<int>(DreamTaskStatus::Completed) == 2, "Completed should be 2");
  Check(static_cast<int>(DreamTaskStatus::Killed) == 3, "Killed should be 3");
  Check(static_cast<int>(DreamTaskStatus::Failed) == 4, "Failed should be 4");
}

// ============================================================================
// Test 11: AutoDreamState defaults
// ============================================================================
void TestAutoDreamStateDefaults() {
  agent::memory::AutoDreamState state;
  Check(state.lastConsolidatedAtMs == 0, "lastConsolidatedAtMs default should be 0");
  Check(state.lastScanAtMs == 0, "lastScanAtMs default should be 0");
  Check(state.enabled == true, "enabled default should be true");
}

// ============================================================================
// Test 12: BuildConsolidationPrompt produces meaningful output
// ============================================================================
void TestBuildConsolidationPrompt() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  std::string prompt = engine.BuildConsolidationPrompt("test extra");
  Check(!prompt.empty(), "BuildConsolidationPrompt should return non-empty string");
  
  // Should reference consolidation/memory concepts
  bool hasKeywords = prompt.find("consolidat") != std::string::npos ||
                     prompt.find("memory") != std::string::npos ||
                     prompt.find("MEMORY") != std::string::npos;
  Check(hasKeywords, "Prompt should contain consolidation/memory keywords");
  RemoveDir(dir);
}

// ============================================================================
// Test 13: Engine construction with nullptr MemoryIndex handled
// ============================================================================
void TestNullMemoryIndex() {
  agent::memory::AutoDreamEngine engine(nullptr, nullptr);
  // Should not crash - should use default paths
  Check(true, "Engine with nullptr MemoryIndex constructs without crash");
}

// ============================================================================
// Test 14: Engine state() returns current state snapshot
// ============================================================================
void TestStateSnapshot() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  agent::memory::AutoDreamState s = engine.state();
  Check(s.enabled == true, "State snapshot should show enabled=true");
  
  engine.Disable();
  agent::memory::AutoDreamState s2 = engine.state();
  Check(s2.enabled == false, "State snapshot should show enabled=false after disable");
  RemoveDir(dir);
}

// ============================================================================
// Test 15: IsSessionGatePassed is callable without crash
// ============================================================================
void TestSessionGateCallable() {
  std::string dir = MakeTempDir();
  agent::memory::MemoryIndex idx(dir);
  agent::memory::AutoDreamEngine engine(&idx, nullptr);
  
  // Session gate checks memory dir for sessions - with empty dir it returns false
  engine.IsSessionGatePassed();
  // May be true or false depending on whether session dirs exist
  // Just verify it doesn't crash
  Check(true, "IsSessionGatePassed completed without crash");
  RemoveDir(dir);
}

}  // namespace

int main() {
  std::cout << "=== AutoDream Tests ===\n";
  
  TestConfigDefaults();
  TestEnableDisable();
  TestConfigure();
  TestTimeGateImmediate();
  TestGateOpenDisabled();
  TestShouldExecuteDisabled();
  TestShouldExecuteTimeGate();
  TestDreamTaskLifecycle();
  TestMultipleTasks();
  TestDreamTaskStatusEnum();
  TestAutoDreamStateDefaults();
  TestBuildConsolidationPrompt();
  TestNullMemoryIndex();
  TestStateSnapshot();
  TestSessionGateCallable();
  
  std::cout << "Failures: " << failures << "\n";
  return failures > 0 ? 1 : 0;
}