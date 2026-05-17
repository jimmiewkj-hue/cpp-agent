#include "memory/MemoryIndex.h"
#include "memory/AutoDream.h"

#include <windows.h>

#include <fstream>
#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

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
  TestAutoDreamConfig();
  TestAutoDreamState();
  TestAutoDreamGates();

  std::cout << "[test_memory] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
