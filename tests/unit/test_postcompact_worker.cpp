#include "compact/PostCompactCleanup.h"
#include "agents/SubAgentWorkerProtocol.h"

#include <cassert>
#include <iostream>
#include <string>

namespace {

int failures = 0;
void Check(bool cond, const char* msg) {
  if (!cond) { std::cerr << "FAIL: " << msg << "\n"; ++failures; }
}

// ============================================================================
// PostCompactCleanup tests
// ============================================================================

// Test 1: Main thread cleanup completes without error
void TestMainThreadCleanup() {
  agent::compact::RunPostCompactCleanup(true);
  Check(agent::compact::IsPostCompactCleanupDone(), "Cleanup should be marked done after main thread call");
  Check(agent::compact::GetCompactCount() > 0, "Compact count should be incremented");
}

// Test 2: Subagent cleanup completes without error
void TestSubagentCleanup() {
  int before = agent::compact::GetCompactCount();
  agent::compact::RunPostCompactCleanup(false);
  // Subagent cleanup still increments count (function is called)
  Check(agent::compact::GetCompactCount() > before, "Subagent cleanup should increment count");
}

// Test 3: Multiple cleanup calls increment count
void TestMultipleCleanups() {
  int before = agent::compact::GetCompactCount();
  agent::compact::RunPostCompactCleanup(true);
  agent::compact::RunPostCompactCleanup(true);
  agent::compact::RunPostCompactCleanup(false);
  Check(agent::compact::GetCompactCount() == before + 3, "Three cleanups should increment count by 3");
}

// Test 4: IsPostCompactCleanupDone stays true after first call
void TestCleanupDonePersistent() {
  agent::compact::RunPostCompactCleanup(true);
  bool first = agent::compact::IsPostCompactCleanupDone();
  bool second = agent::compact::IsPostCompactCleanupDone();
  Check(first && second, "IsPostCompactCleanupDone should stay true");
}

// ============================================================================
// SubAgentWorkerProtocol tests
// ============================================================================

// Test 5: WorkerTaskState enum values
void TestWorkerTaskStateEnum() {
  using agent::agents::WorkerTaskState;
  Check(static_cast<int>(WorkerTaskState::Running) == 0, "Running should be 0");
  Check(static_cast<int>(WorkerTaskState::Completed) == 1, "Completed should be 1");
  Check(static_cast<int>(WorkerTaskState::Preempted) == 2, "Preempted should be 2");
  Check(static_cast<int>(WorkerTaskState::Failed) == 3, "Failed should be 3");
}

// Test 6: Serialize/Deserialize WorkerRequest round-trip
void TestWorkerRequestRoundTrip() {
  agent::agents::SubAgentWorkerRequest req;
  req.taskId = "task-abc-123";
  req.executorId = "executor-xyz";
  req.prompt = "Write a Python script to process CSV data";
  req.priority = 5;
  req.checkpointId = "ckpt-001";
  req.resumeCursor = "line:42";

  std::string serialized = agent::agents::SerializeWorkerRequest(req);
  Check(!serialized.empty(), "Serialized request should not be empty");
  // ProtoLite binary encoding - verify non-empty output, round-trip tested below

  agent::agents::SubAgentWorkerRequest deserialized;
  bool ok = agent::agents::DeserializeWorkerRequest(serialized, &deserialized);
  Check(ok, "DeserializeWorkerRequest should succeed");
  Check(deserialized.taskId == "task-abc-123", "Deserialized taskId should match");
  Check(deserialized.executorId == "executor-xyz", "Deserialized executorId should match");
  Check(deserialized.prompt == "Write a Python script to process CSV data", "Deserialized prompt should match");
  Check(deserialized.priority == 5, "Deserialized priority should match");
  Check(deserialized.checkpointId == "ckpt-001", "Deserialized checkpointId should match");
  Check(deserialized.resumeCursor == "line:42", "Deserialized resumeCursor should match");
}

// Test 7: Serialize/Deserialize WorkerStatus round-trip
void TestWorkerStatusRoundTrip() {
  agent::agents::SubAgentWorkerStatus status;
  status.taskId = "task-def-456";
  status.state = agent::agents::WorkerTaskState::Completed;
  status.completedUnits = 42;
  status.totalUnits = 100;
  status.checkpointId = "ckpt-final";
  status.resumeCursor = "";
  status.summary = "Processed 42/100 units successfully";
  status.updatedAtUnixMs = 1717200000000;

  std::string serialized = agent::agents::SerializeWorkerStatus(status);
  Check(!serialized.empty(), "Serialized status should not be empty");
  // ProtoLite uses binary encoding, not JSON. Verify serialized is non-empty
  // and round-trip deserialization is correct (already checked below).

  agent::agents::SubAgentWorkerStatus deserialized;
  bool ok = agent::agents::DeserializeWorkerStatus(serialized, &deserialized);
  Check(ok, "DeserializeWorkerStatus should succeed");
  Check(deserialized.taskId == "task-def-456", "Deserialized taskId should match");
  Check(deserialized.state == agent::agents::WorkerTaskState::Completed, "Deserialized state should be Completed");
  Check(deserialized.completedUnits == 42, "Deserialized completedUnits should match");
  Check(deserialized.totalUnits == 100, "Deserialized totalUnits should match");
  Check(deserialized.checkpointId == "ckpt-final", "Deserialized checkpointId should match");
}

// Test 8: Serialize/Deserialize WorkerStatus with Running state
void TestWorkerStatusRunning() {
  agent::agents::SubAgentWorkerStatus status;
  status.taskId = "task-ghi-789";
  status.state = agent::agents::WorkerTaskState::Running;
  status.completedUnits = 10;
  status.totalUnits = 50;
  status.summary = "In progress...";

  std::string serialized = agent::agents::SerializeWorkerStatus(status);
  agent::agents::SubAgentWorkerStatus deserialized;
  bool ok = agent::agents::DeserializeWorkerStatus(serialized, &deserialized);
  Check(ok, "DeserializeWorkerStatus should succeed for Running state");
  Check(deserialized.state == agent::agents::WorkerTaskState::Running, "State should be Running");
  Check(deserialized.completedUnits == 10, "completedUnits should be 10");
  Check(deserialized.totalUnits == 50, "totalUnits should be 50");
}

// Test 9: Serialize/Deserialize WorkerStatus with Failed state
void TestWorkerStatusFailed() {
  agent::agents::SubAgentWorkerStatus status;
  status.taskId = "task-jkl-000";
  status.state = agent::agents::WorkerTaskState::Failed;
  status.summary = "Task failed due to timeout";

  std::string serialized = agent::agents::SerializeWorkerStatus(status);
  agent::agents::SubAgentWorkerStatus deserialized;
  bool ok = agent::agents::DeserializeWorkerStatus(serialized, &deserialized);
  Check(ok, "DeserializeWorkerStatus should succeed for Failed state");
  Check(deserialized.state == agent::agents::WorkerTaskState::Failed, "State should be Failed");
  Check(deserialized.summary == "Task failed due to timeout", "summary should match");
}

// Test 10: DeserializeWorkerRequest with nullptr returns false
void TestDeserializeNullRequest() {
  std::string data = "{}";
  bool ok = agent::agents::DeserializeWorkerRequest(data, nullptr);
  Check(!ok, "DeserializeWorkerRequest should return false for nullptr");
}

// Test 11: DeserializeWorkerStatus with nullptr returns false
void TestDeserializeNullStatus() {
  std::string data = "{}";
  bool ok = agent::agents::DeserializeWorkerStatus(data, nullptr);
  Check(!ok, "DeserializeWorkerStatus should return false for nullptr");
}

// Test 12: DeserializeWorkerRequest with invalid data returns false
void TestDeserializeInvalidRequest() {
  agent::agents::SubAgentWorkerRequest req;
  bool ok = agent::agents::DeserializeWorkerRequest("not-valid-json", &req);
  Check(!ok, "DeserializeWorkerRequest should return false for invalid data");
}

// Test 13: SubAgentWorkerRequest default values
void TestWorkerRequestDefaults() {
  agent::agents::SubAgentWorkerRequest req;
  Check(req.priority == 0, "Default priority should be 0");
  Check(req.taskId.empty(), "Default taskId should be empty");
  Check(req.executorId.empty(), "Default executorId should be empty");
}

// Test 14: SubAgentWorkerStatus default values
void TestWorkerStatusDefaults() {
  agent::agents::SubAgentWorkerStatus status;
  Check(status.state == agent::agents::WorkerTaskState::Running, "Default state should be Running");
  Check(status.completedUnits == 0, "Default completedUnits should be 0");
  Check(status.totalUnits == 0, "Default totalUnits should be 0");
  Check(status.updatedAtUnixMs == 0, "Default updatedAtUnixMs should be 0");
}

// Test 15: Preempted state serialization round-trip
void TestWorkerStatusPreempted() {
  agent::agents::SubAgentWorkerStatus status;
  status.taskId = "task-preempt";
  status.state = agent::agents::WorkerTaskState::Preempted;
  status.completedUnits = 30;
  status.totalUnits = 60;
  status.resumeCursor = "checkpoint:line:50";

  std::string serialized = agent::agents::SerializeWorkerStatus(status);
  agent::agents::SubAgentWorkerStatus deserialized;
  bool ok = agent::agents::DeserializeWorkerStatus(serialized, &deserialized);
  Check(ok, "DeserializeWorkerStatus should succeed for Preempted state");
  Check(deserialized.state == agent::agents::WorkerTaskState::Preempted, "State should be Preempted");
  Check(deserialized.resumeCursor == "checkpoint:line:50", "resumeCursor should match");
}

}  // namespace

int main() {
  std::cout << "=== PostCompactCleanup + SubAgentWorkerProtocol Tests ===\n";

  TestMainThreadCleanup();
  TestSubagentCleanup();
  TestMultipleCleanups();
  TestCleanupDonePersistent();

  TestWorkerTaskStateEnum();
  TestWorkerRequestRoundTrip();
  TestWorkerStatusRoundTrip();
  TestWorkerStatusRunning();
  TestWorkerStatusFailed();
  TestDeserializeNullRequest();
  TestDeserializeNullStatus();
  TestDeserializeInvalidRequest();
  TestWorkerRequestDefaults();
  TestWorkerStatusDefaults();
  TestWorkerStatusPreempted();

  std::cout << "Failures: " << failures << "\n";
  return failures > 0 ? 1 : 0;
}