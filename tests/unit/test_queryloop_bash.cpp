#include "core/QueryEngine.h"
#include "core/QueryLoop.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "infra/SessionManager.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"
#include "third_party/nlohmann_json.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

// Test 1: Verify grep/head/tail conversion to PowerShell equivalents
void TestGrepHeadTailConversion() {
  agent::tools::ToolRegistry registry;
  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&registry);

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  // Test grep conversion
  {
    nlohmann::json cmd;
    cmd["command"] = "grep TODO test.txt";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("grep1", "Bash", cmd.dump())},
        canUse, {});
    bool hasSelectString = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("Select-String") != std::string::npos) {
          hasSelectString = true;
        }
      }
    }
    Check(hasSelectString, "grep is converted to Select-String");
  }

  // Test grep -i conversion (case insensitive)
  {
    nlohmann::json cmd;
    cmd["command"] = "grep -i TODO test.txt";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("grep2", "Bash", cmd.dump())},
        canUse, {});
    bool hasCaseInsensitive = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("CaseSensitive") != std::string::npos) {
          hasCaseInsensitive = true;
        }
      }
    }
    Check(hasCaseInsensitive, "grep -i includes -CaseSensitive:$false");
  }

  // Test head conversion
  {
    nlohmann::json cmd;
    cmd["command"] = "head -20 test.txt";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("head1", "Bash", cmd.dump())},
        canUse, {});
    bool hasFirst20 = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("First 20") != std::string::npos) {
          hasFirst20 = true;
        }
      }
    }
    Check(hasFirst20, "head -20 is converted to Select-Object -First 20");
  }

  // Test tail conversion
  {
    nlohmann::json cmd;
    cmd["command"] = "tail -50 test.txt";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("tail1", "Bash", cmd.dump())},
        canUse, {});
    bool hasLast50 = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("Last 50") != std::string::npos) {
          hasLast50 = true;
        }
      }
    }
    Check(hasLast50, "tail -50 is converted to Select-Object -Last 50");
  }

  // Test piped grep
  {
    nlohmann::json cmd;
    cmd["command"] = "python -m pip list 2>&1 | grep requests";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("pipe1", "Bash", cmd.dump())},
        canUse, {});
    bool hasSelectStringPipe = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("Select-String") != std::string::npos) {
          hasSelectStringPipe = true;
        }
      }
    }
    Check(hasSelectStringPipe, "piped grep is converted to Select-String");
  }

  // Test piped head
  {
    nlohmann::json cmd;
    cmd["command"] = "pip list | head -10";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("pipe2", "Bash", cmd.dump())},
        canUse, {});
    bool hasFirst10 = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("First 10") != std::string::npos) {
          hasFirst10 = true;
        }
      }
    }
    Check(hasFirst10, "piped head -10 is converted to Select-Object -First 10");
  }

  // Test piped tail
  {
    nlohmann::json cmd;
    cmd["command"] = "pip list | tail -5";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse("pipe3", "Bash", cmd.dump())},
        canUse, {});
    bool hasLast5 = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("Last 5") != std::string::npos) {
          hasLast5 = true;
        }
      }
    }
    Check(hasLast5, "piped tail -5 is converted to Select-Object -Last 5");
  }

  std::cout << "[test_queryloop_bash] Failures: " << failures << std::endl;
}

int main() {
  TestGrepHeadTailConversion();

  if (failures == 0) {
    std::cout << "All tests passed." << std::endl;
    return 0;
  }
  return 1;
}
