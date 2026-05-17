#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"
#include "permissions/PermissionEngine.h"

#include <windows.h>

#include <fstream>
#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

void TestToolRegistry() {
  agent::tools::ToolRegistry registry;

  agent::tools::ToolSchema bash;
  bash.name = "Bash";
  bash.description = "Execute shell command";
  bash.category = agent::tools::ToolExecCategory::ShellCommand;
  bash.readOnlyHint = false;
  bash.destructiveHint = true;
  registry.RegisterTool(bash);

  agent::tools::ToolSchema fileRead;
  fileRead.name = "FileRead";
  fileRead.description = "Read a file";
  fileRead.category = agent::tools::ToolExecCategory::ReadOnly;
  fileRead.readOnlyHint = true;
  registry.RegisterTool(fileRead);

  const auto tools = registry.ListTools();
  Check(tools.size() >= 2, "ToolRegistry should list registered tools");
  Check(registry.IsConcurrencySafe("FileRead"), "FileRead concurrency safe");
  Check(!registry.IsConcurrencySafe("Bash"), "Bash not concurrency safe");
}

void TestToolPartition() {
  agent::tools::ToolRegistry registry;
  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&registry);

  agent::tools::ToolSchema read;
  read.name = "FileRead";
  read.readOnlyHint = true;
  registry.RegisterTool(read);

  agent::tools::ToolSchema write;
  write.name = "Bash";
  write.destructiveHint = true;
  registry.RegisterTool(write);

  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse("tu-1", "FileRead", "{}"));
  blocks.push_back(agent::core::ContentBlock::MakeToolUse("tu-2", "FileRead", "{}"));
  blocks.push_back(agent::core::ContentBlock::MakeToolUse("tu-3", "Bash", "{}"));

  auto batches = orchestrator.PartitionToolCalls(blocks);
  Check(batches.size() >= 2, "Partition should separate read/write batches");
}

void TestRealBash() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "bash-1", "Bash", R"({"command":"echo hello-p3"})"));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(), "Bash should produce result");
}

void TestRealFileRead() {
  std::string tmpPath = "build\\p3_test_ft.txt";
  { std::ofstream out(tmpPath); out << "P3 test content.\n"; }

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fr-1", "FileRead", R"({"file_path":")" + tmpPath + R"("})"));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(), "FileRead should produce result");
}

void TestDeniedExecution() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "deny-1", "Bash", R"({"command":"rm"})"));
  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Deny;
    d.reason = "dangerous";
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(result.deniedCount > 0, "Denied tool increments deniedCount");
}

}  // namespace

int main() {
  TestToolRegistry();
  TestToolPartition();
  TestRealBash();
  TestRealFileRead();
  TestDeniedExecution();
  std::cout << "[test_tools] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
