// Test ConsolidationLock + ConsolidationPrompt — aligned with local-ace autoDream
#include "memory/ConsolidationLock.h"
#include "memory/ConsolidationPrompt.h"

#include <cassert>
#include <iostream>
#include <string>
#include <windows.h>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

static std::string MakeTempDir() {
  char buf[MAX_PATH];
  GetTempPathA(sizeof(buf), buf);
  std::string dir = std::string(buf) + "cpp_agent_test_consolidation";
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir;
}

static void RemoveTempDir(const std::string& dir) {
  // Simple cleanup
  DeleteFileA((dir + "\\.consolidate-lock").c_str());
  RemoveDirectoryA(dir.c_str());
}

// ============================================================================
// ConsolidationLock Tests
// ============================================================================
void TestLockPath() {
  std::string path = agent::memory::GetConsolidationLockPath("C:\\test\\memdir");
  Check(path.find(".consolidate-lock") != std::string::npos,
        "Lock path contains lock filename");
  Check(path.find("C:\\test\\memdir") != std::string::npos,
        "Lock path contains memory dir");
  
  std::string emptyPath = agent::memory::GetConsolidationLockPath("");
  Check(emptyPath == ".consolidate-lock",
        "Empty dir returns relative lock path");
}

void TestReadLastConsolidatedAt() {
  std::string dir = MakeTempDir();
  
  // No lock file → 0
  long long mtime = agent::memory::ReadLastConsolidatedAt(dir);
  Check(mtime == 0, "No lock file → mtime = 0");
  
  RemoveTempDir(dir);
}

void TestLockLifecycle() {
  std::string dir = MakeTempDir();
  
  // Initially no lock → expired
  Check(agent::memory::IsConsolidationLockExpired(dir),
        "No lock → IsExpired = true");
  
  // Acquire lock
  long long priorMtime = 0;
  bool acquired = agent::memory::TryAcquireConsolidationLock(dir, &priorMtime);
  Check(acquired, "Acquire lock succeeds");
  Check(priorMtime == 0, "Prior mtime is 0 for first acquisition");
  
  // After acquisition, should NOT be expired
  Check(!agent::memory::IsConsolidationLockExpired(dir),
        "After acquire → IsExpired = false");
  
  // Read should return non-zero
  long long mtime = agent::memory::ReadLastConsolidatedAt(dir);
  Check(mtime > 0, "After acquire → mtime > 0");
  
  // Release
  agent::memory::ReleaseConsolidationLock(dir);
  
  // After release → expired
  Check(agent::memory::IsConsolidationLockExpired(dir),
        "After release → IsExpired = true");
  
  RemoveTempDir(dir);
}

void TestRollback() {
  std::string dir = MakeTempDir();
  
  // Acquire
  long long priorMtime = 0;
  agent::memory::TryAcquireConsolidationLock(dir, &priorMtime);
  
  // Rollback with priorMtime=0 → delete
  agent::memory::RollbackConsolidationLock(dir, 0);
  
  // Should be expired
  Check(agent::memory::IsConsolidationLockExpired(dir),
        "After rollback(0) → IsExpired = true");
  
  // Acquire again
  long long prior2 = 0;
  agent::memory::TryAcquireConsolidationLock(dir, &prior2);
  Check(prior2 == 0, "After rollback, prior mtime is 0");
  
  // Rollback with non-zero prior
  long long current = agent::memory::ReadLastConsolidatedAt(dir);
  agent::memory::RollbackConsolidationLock(dir, prior2);  // rollback to 0
  
  RemoveTempDir(dir);
}

void TestRecordConsolidation() {
  std::string dir = MakeTempDir();
  
  agent::memory::RecordConsolidation(dir);
  
  // Should have a lock file now
  Check(!agent::memory::IsConsolidationLockExpired(dir),
        "After RecordConsolidation → not expired");
  
  RemoveTempDir(dir);
}

// ============================================================================
// ConsolidationPrompt Tests
// ============================================================================
void TestBuildConsolidationPrompt() {
  std::string prompt = agent::memory::BuildConsolidationPrompt(
      "C:\\memories", "C:\\transcripts");
  
  Check(!prompt.empty(), "Prompt is non-empty");
  Check(prompt.find("# Dream: Memory Consolidation") != std::string::npos,
        "Prompt starts with Dream header");
  Check(prompt.find("C:\\memories") != std::string::npos,
        "Prompt contains memory directory");
  Check(prompt.find("C:\\transcripts") != std::string::npos,
        "Prompt contains transcript directory");
  Check(prompt.find("Phase 1 — Orient") != std::string::npos,
        "Prompt has Phase 1");
  Check(prompt.find("Phase 2 — Gather") != std::string::npos,
        "Prompt has Phase 2");
  Check(prompt.find("Phase 3 — Consolidate") != std::string::npos,
        "Prompt has Phase 3");
  Check(prompt.find("Phase 4 — Prune") != std::string::npos,
        "Prompt has Phase 4");
  Check(prompt.find("MEMORY.md") != std::string::npos,
        "Prompt mentions entrypoint file");
}

void TestBuildConsolidationPromptWithExtra() {
  std::string prompt = agent::memory::BuildConsolidationPrompt(
      "C:\\memories", "C:\\transcripts",
      "Focus on TypeScript patterns and build configurations.");
  
  Check(prompt.find("Additional context") != std::string::npos,
        "Prompt with extra has Additional context section");
  Check(prompt.find("TypeScript patterns") != std::string::npos,
        "Prompt with extra contains the extra text");
}

void TestConstants() {
  // kHolderStaleMs should be 1 hour
  Check(agent::memory::kHolderStaleMs == 60 * 60 * 1000,
        "kHolderStaleMs = 1 hour");
}

int main() {
  std::cout << "=== Consolidation Module Tests ===" << std::endl;
  
  TestLockPath();
  TestReadLastConsolidatedAt();
  TestLockLifecycle();
  TestRollback();
  TestRecordConsolidation();
  TestBuildConsolidationPrompt();
  TestBuildConsolidationPromptWithExtra();
  TestConstants();
  
  std::cout << "=== " << failures << " failure(s) ===" << std::endl;
  return failures > 0 ? 1 : 0;
}
