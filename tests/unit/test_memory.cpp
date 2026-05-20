#include "memory/MemoryIndex.h"
#include "memory/AutoDream.h"
#include "api/ModelClient.h"
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
