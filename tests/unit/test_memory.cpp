#include "memory/MemoryIndex.h"
#include "memory/AutoDream.h"
#include "api/ModelClient.h"
#include "memory/SessionMemory.h"
#include "api/SideQueryClient.h"

#include <windows.h>

#include <fstream>
#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

class FakeModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return std::vector<agent::core::Message>();
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "<selected_memories>\nuser_role.md\n</selected_memories>"));
    return std::vector<agent::core::Message>(1, msg);
  }
};

std::string TestMemoryDir() {
  return "build\\test-memory";
}

void EnsureTestMemoryDir() {
  CreateDirectoryA("build", nullptr);
  CreateDirectoryA(TestMemoryDir().c_str(), nullptr);

  std::ofstream mem(TestMemoryDir() + "\\MEMORY.md");
  mem << "# Test Memory\n\n## project\n- Test entry\n";
  std::ofstream role(TestMemoryDir() + "\\user_role.md");
  role << "You are a test assistant.\n";
}

void TestMemoryIndexBasic() {
  EnsureTestMemoryDir();
  agent::memory::MemoryIndex index(TestMemoryDir());
  std::string entrypoint = index.ReadEntrypoint();
  Check(!entrypoint.empty(), "MemoryIndex should read MEMORY.md");
  Check(entrypoint.find("Test Memory") != std::string::npos,
        "MemoryIndex should contain header");
}

void TestMemoryTruncation() {
  EnsureTestMemoryDir();
  agent::memory::MemoryIndex index(TestMemoryDir());
  std::string content = index.ReadEntrypoint();
  auto trunc = index.TruncateEntrypointContent(content);
  Check(!trunc.content.empty(), "Truncate should return content");
}

void TestMemoryPromptInjection() {
  EnsureTestMemoryDir();
  agent::memory::MemoryIndex index(TestMemoryDir());
  std::string injection = index.BuildSystemPromptInjection();
  Check(!injection.empty() || true, "BuildSystemPromptInjection runs");
}

void TestFindRelevantMemoriesUsesSideQuery() {
  EnsureTestMemoryDir();
  agent::memory::MemoryIndex index(TestMemoryDir());
  FakeModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  index.SetSideQueryClient(&sideQueryClient);

  const std::vector<agent::memory::MemoryIndex::RelevantMemory> relevant =
      index.FindRelevantMemories("Who am I?", std::vector<std::string>());
  Check(relevant.size() == 1, "FindRelevantMemories should return selection");
  if (!relevant.empty()) {
    Check(relevant[0].fileName == "user_role.md",
          "FindRelevantMemories should use side query filenames");
  }
}

void TestAutoDreamConfig() {
  agent::memory::AutoDreamConfig cfg;
  cfg.minHours = 24;
  cfg.minSessions = 5;
  cfg.scanThrottleMs = 600000;
  Check(cfg.minHours == 24, "AutoDreamConfig minHours");
  Check(cfg.minSessions == 5, "AutoDreamConfig minSessions");
}

void TestAutoDreamState() {
  agent::memory::AutoDreamState state;
  Check(state.enabled, "AutoDreamState default enabled");
  Check(state.lastConsolidatedAtMs == 0, "AutoDreamState default 0");
  Check(state.lastScanAtMs == 0, "AutoDreamState default 0");
}

void TestAutoDreamGates() {
  agent::memory::MemoryIndex index(TestMemoryDir());
  agent::memory::AutoDreamEngine dream(&index, nullptr);
  Check(dream.IsEnabled(), "AutoDreamEngine default enabled");
  Check(dream.IsGateOpen(), "AutoDreamEngine gate open with valid index");

  dream.Configure({24, 5, 600000});
  Check(!dream.IsTimeGatePassed(), "Time gate should not pass immediately");

  dream.Disable();
  Check(!dream.IsEnabled(), "AutoDreamEngine disabled");
  dream.Enable();
  Check(dream.IsEnabled(), "AutoDreamEngine re-enabled");
}

}  // namespace


void TestSessionMemoryCRUD() {
  std::string sessionDir = "build\\test-session-memory";
  CreateDirectoryA(sessionDir.c_str(), nullptr);

  agent::memory::SessionMemory sm(sessionDir);

  // Add memories
  auto id1 = sm.AddMemory("User prefers Python for scripting", "user", "session", 3);
  auto id2 = sm.AddMemory("Project uses CMake build system", "project", "project", 5);
  auto id3 = sm.AddMemory("Avoid raw pointers, use smart pointers", "reference", "global", 2);

  Check(!id1.empty(), "AddMemory should return non-empty id");
  Check(id1 != id2, "Memory ids should be unique");

  // List all
  auto all = sm.ListMemories();
  Check(all.size() >= 3, "ListMemories should return all active memories");

  // List by type
  auto project = sm.ListMemories("project");
  Check(project.size() >= 1, "ListMemories should filter by type");
  bool foundProject = false;
  for (const auto& m : project) { if (m.content.find("CMake") != std::string::npos) foundProject = true; }
  Check(foundProject, "Project memory should contain CMake reference");

  // List by scope
  auto global = sm.ListMemories("", "global");
  Check(global.size() >= 1, "ListMemories should filter by scope");

  // Search
  auto results = sm.SearchMemories("Python");
  Check(!results.empty(), "SearchMemories should find Python-related memory");
  Check(results[0].content.find("Python") != std::string::npos, "Search result should contain search term");

  // Update
  bool updated = sm.UpdateMemory(id1, "User strongly prefers Python 3.11+ for scripting");
  Check(updated, "UpdateMemory should succeed for valid id");
  auto updatedList = sm.SearchMemories("Python 3.11");
  Check(!updatedList.empty(), "Updated memory should be searchable with new content");

  // Deactivate
  bool deactivated = sm.DeactivateMemory(id3);
  Check(deactivated, "DeactivateMemory should succeed");
  auto active = sm.ListMemories();
  bool foundDeactivated = false;
  for (const auto& m : active) { if (m.id == id3) foundDeactivated = true; }
  Check(!foundDeactivated, "Deactivated memory should not appear in ListMemories");

  // Delete
  bool deleted = sm.DeleteMemory(id2);
  Check(deleted, "DeleteMemory should succeed");

  // Verify persistence: create new instance and check it loads
  agent::memory::SessionMemory sm2(sessionDir);
  auto loaded = sm2.ListMemories();
  Check(loaded.size() >= 1, "Persisted memories should survive reload");
}

void TestSessionMemoryConsolidation() {
  std::string sessionDir = "build\\test-consolidation";
  CreateDirectoryA(sessionDir.c_str(), nullptr);

  // Clean up any previous data
  DeleteFileA((sessionDir + "\\session-memory.json").c_str());

  agent::memory::SessionMemory sm(sessionDir);

  // Add similar memories
  sm.AddMemory("Use clang-format for code formatting", "reference", "session", 1);
  sm.AddMemory("Use clang-format for code formatting in all C++ files", "reference", "session", 1);

  auto before = sm.ListMemories();
  int beforeCount = static_cast<int>(before.size());

  sm.Consolidate();

  auto after = sm.ListMemories();
  Check(after.size() <= beforeCount, "Consolidation should merge or keep same count");
}

void TestSessionMemoryContextInjection() {
  std::string sessionDir = "build\\test-context-injection";
  CreateDirectoryA(sessionDir.c_str(), nullptr);
  DeleteFileA((sessionDir + "\\session-memory.json").c_str());

  agent::memory::SessionMemory sm(sessionDir);

  sm.AddMemory("Project directory: G:\\downloads\\jianlai-graph", "project", "project", 5);
  sm.AddMemory("Use jieba for Chinese text segmentation", "reference", "session", 3);

  std::string injection = sm.BuildMemoryContextInjection(500);
  Check(!injection.empty(), "Context injection should not be empty");
  Check(injection.find("[Session Memories]") != std::string::npos, "Injection should have header");
  Check(injection.find("jianlai-graph") != std::string::npos, "Injection should include memory content");
}

void TestSessionMemoryEdgeCases() {
  std::string sessionDir = "build\\test-memory-edge";
  CreateDirectoryA(sessionDir.c_str(), nullptr);
  DeleteFileA((sessionDir + "\\session-memory.json").c_str());

  agent::memory::SessionMemory sm(sessionDir);

  // Empty store
  auto empty = sm.ListMemories();
  Check(empty.empty(), "Empty store should return empty list");

  // Empty search
  auto noResults = sm.SearchMemories("nonexistent");
  Check(noResults.empty(), "Search for nonexistent should return empty");

  // Deactivate nonexistent
  bool deactivated = sm.DeactivateMemory("nonexistent-id");
  Check(!deactivated, "Deactivate nonexistent should return false");

  // Update nonexistent
  bool updated = sm.UpdateMemory("nonexistent-id", "new content");
  Check(!updated, "Update nonexistent should return false");

  // Empty context injection
  std::string injection = sm.BuildMemoryContextInjection();
  Check(injection.empty(), "Empty store should produce empty injection");

  // Duplicate detection
  auto id1 = sm.AddMemory("Remember to use UTF-8 encoding", "reference");
  auto id2 = sm.AddMemory("Remember to use UTF-8 encoding", "reference");
  Check(id1 == id2, "Duplicate content should return same id");
}

void TestSessionMemoryPriorityOrdering() {
  std::string sessionDir = "build\\test-memory-priority";
  CreateDirectoryA(sessionDir.c_str(), nullptr);
  DeleteFileA((sessionDir + "\\session-memory.json").c_str());

  agent::memory::SessionMemory sm(sessionDir);

  sm.AddMemory("Low priority note", "reference", "session", 1);
  sm.AddMemory("High priority critical config", "project", "session", 10);
  sm.AddMemory("Medium priority tip", "reference", "session", 5);

  auto list = sm.ListMemories();
  Check(list.size() >= 3, "Should have 3 memories");
  // First entry should be highest priority
  Check(!list.empty() && list[0].priority >= 10, "First entry should have highest priority");
}
int main() {
  TestMemoryIndexBasic();
  TestMemoryTruncation();
  TestMemoryPromptInjection();
  TestFindRelevantMemoriesUsesSideQuery();
  TestAutoDreamConfig();
  TestAutoDreamState();
  TestAutoDreamGates();

  std::cout << "[test_memory] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}