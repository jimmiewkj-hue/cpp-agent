#include "app/TuiTaskPanel.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"
#include "agents/SubAgentManager.h"
#include "permissions/PermissionEngine.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>

#include <fstream>
#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int size = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(size), L'\0');
  MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &wide[0], size);
  return wide;
}

std::string FullPathOf(const std::string& path) {
  char buffer[MAX_PATH] = {0};
  DWORD length = GetFullPathNameA(path.c_str(), MAX_PATH, buffer, nullptr);
  if (length == 0 || length >= MAX_PATH) return path;
  return std::string(buffer, length);
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

void TestWindowsLsCompatibility() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "bash-ls-1", "Bash", R"({"command":"ls -la"})"));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(),
        "Windows ls compatibility should produce result");

  bool sawNormalized = false;
  bool sawExitFailure = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.content.find(
              "[normalized command] Get-ChildItem -Force | Select-Object Mode,LastWriteTime,Length,Name") !=
          std::string::npos) {
        sawNormalized = true;
      }
      if (block.asToolResult.content.find("[exit code:") != std::string::npos) {
        sawExitFailure = true;
      }
    }
  }
  Check(sawNormalized,
        "Windows ls compatibility should normalize ls -la");
  Check(!sawExitFailure,
        "Windows ls compatibility should avoid shell failure");
}

void TestWindowsDirCompatibility() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "bash-dir-1", "Bash", R"({"command":"dir /b /a-d ."})"));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(),
        "Windows dir compatibility should produce result");

  bool sawNormalized = false;
  bool sawExitFailure = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.content.find(
              "[normalized command] Get-ChildItem -File -Path '.' | ForEach-Object { $_.Name }") !=
          std::string::npos) {
        sawNormalized = true;
      }
      if (block.asToolResult.content.find("[exit code:") != std::string::npos) {
        sawExitFailure = true;
      }
    }
  }
  Check(sawNormalized,
        "Windows dir compatibility should normalize dir /b /a-d");
  Check(!sawExitFailure,
        "Windows dir compatibility should avoid shell failure");
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

void TestFileReadSupportsOffsetLimit() {
  const std::string tmpPath = "build\\p3_read_range_test.txt";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    out << "line-1\nline-2\nline-3\nline-4\n";
  }

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  nlohmann::json readInput;
  readInput["file_path"] = tmpPath;
  readInput["offset"] = 2;
  readInput["limit"] = 2;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fr-range-1", "Read", readInput.dump()));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(), "Ranged FileRead should produce result");

  bool sawStartLine = false;
  bool sawLine2 = false;
  bool sawLine3 = false;
  bool sawLine1 = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      const std::string& content = block.asToolResult.content;
      if (content.find("start_line=\"2\"") != std::string::npos) {
        sawStartLine = true;
      }
      if (content.find("2->line-2") != std::string::npos) {
        sawLine2 = true;
      }
      if (content.find("3->line-3") != std::string::npos) {
        sawLine3 = true;
      }
      if (content.find("1->line-1") != std::string::npos) {
        sawLine1 = true;
      }
    }
  }

  Check(sawStartLine, "Ranged FileRead should record the requested start line");
  Check(sawLine2, "Ranged FileRead should include the first requested line");
  Check(sawLine3, "Ranged FileRead should include the second requested line");
  Check(!sawLine1, "Ranged FileRead should not include lines before offset");

  DeleteFileA(tmpPath.c_str());
}

void TestLargeFileReadRequiresTargetedRange() {
  const std::string tmpPath = "build\\p3_large_read_test.txt";
  {
    std::ofstream out(tmpPath, std::ios::binary);
    for (int i = 0; i < 40000; ++i) {
      out << "line-" << i << " abcdefghijklmnopqrstuvwxyz\n";
    }
  }

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  nlohmann::json readInput;
  readInput["file_path"] = tmpPath;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fr-large-1", "Read", readInput.dump()));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(),
        "Large FileRead should still produce a tool result");

  bool sawGuidance = false;
  bool sawError = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.content.find(
              "Use Read with offset/limit to inspect a targeted line range") !=
          std::string::npos) {
        sawGuidance = true;
      }
      if (block.asToolResult.isError) {
        sawError = true;
      }
    }
  }

  Check(sawGuidance,
        "Large FileRead should instruct the model to use offset/limit");
  Check(sawError, "Large FileRead guidance should be surfaced as an error");

  DeleteFileA(tmpPath.c_str());
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

void TestAskExecutionBehavesAsDeniedInNonInteractiveMode() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "ask-1", "Bash", R"({"command":"python --version"})"));
  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Ask;
    d.reason = "no rule matched; requires confirmation";
    return d;
  };

  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(result.deniedCount == 1,
        "Ask tool result should count as denied in non-interactive mode");
  Check(result.errorCount == 1,
        "Ask tool result should count as error in non-interactive mode");

  bool sawErrorToolResult = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.isError &&
          block.asToolResult.content.find("requires confirmation") !=
              std::string::npos) {
        sawErrorToolResult = true;
      }
    }
  }
  Check(sawErrorToolResult,
        "Ask tool result should be surfaced as an error result");
}

void TestRealFileWrite() {
  std::string tmpPath = "build\\p3_filewrite_test.txt";
  std::string testContent = "Hello from FileWrite tool!\nLine 2 content.\n";

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;

  nlohmann::json inputJson;
  inputJson["file_path"] = tmpPath;
  inputJson["content"] = testContent;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fw-1", "FileWrite", inputJson.dump()));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(!result.userMessages.empty(), "FileWrite should produce result");
  Check(result.errorCount == 0, "FileWrite should not error");

  bool foundCreateMsg = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find("Created file") != std::string::npos) {
        foundCreateMsg = true;
      }
    }
  }
  Check(foundCreateMsg, "FileWrite result should say Created file");

  std::ifstream verify(tmpPath, std::ios::binary);
  Check(verify.good(), "FileWrite output file should exist on disk");
  std::string actualContent((std::istreambuf_iterator<char>(verify)),
                             std::istreambuf_iterator<char>());
  verify.close();
  Check(actualContent == testContent, "FileWrite content should match on disk");

  DeleteFileA(tmpPath.c_str());
}

void TestRealFileWriteOverwrite() {
  std::string tmpPath = "build\\p3_filewrite_overwrite.txt";
  std::string firstContent = "Version 1.\n";
  std::string secondContent = "Version 2 - updated.\n";

  {
    std::ofstream out(tmpPath, std::ios::binary);
    out << firstContent;
    out.close();
  }

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;

  nlohmann::json inputJson;
  inputJson["file_path"] = tmpPath;
  inputJson["content"] = secondContent;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fw-2", "FileWrite", inputJson.dump()));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(result.errorCount == 0, "FileWrite overwrite should not error");

  bool foundUpdateMsg = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find("Updated file") != std::string::npos) {
        foundUpdateMsg = true;
      }
    }
  }
  Check(foundUpdateMsg, "FileWrite result should say Updated file for overwrite");

  std::ifstream verify(tmpPath, std::ios::binary);
  Check(verify.good(), "FileWrite overwrite output should exist on disk");
  std::string actualContent((std::istreambuf_iterator<char>(verify)),
                             std::istreambuf_iterator<char>());
  verify.close();
  Check(actualContent == secondContent, "FileWrite overwrite content should match on disk");

  DeleteFileA(tmpPath.c_str());
}

void TestRealFileWriteEdgeCases() {
  {
    std::string tmpPath = "build\\p3 edge file.txt";
    std::string content = "File with spaces in path.\n";

    agent::tools::ToolOrchestrator orchestrator;
    std::vector<agent::core::ContentBlock> blocks;

    nlohmann::json inputJson;
    inputJson["file_path"] = tmpPath;
    inputJson["content"] = content;
    blocks.push_back(agent::core::ContentBlock::MakeToolUse(
        "fw-edge-1", "FileWrite", inputJson.dump()));

    std::vector<agent::core::Message> messages;
    agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                         const std::vector<agent::core::Message>&) {
      agent::core::PermissionDecision d;
      d.behavior = agent::core::PermissionBehavior::Allow;
      return d;
    };
    auto result = orchestrator.Execute(blocks, canUse, messages);
    Check(result.errorCount == 0, "FileWrite with spaces in path should not error");

    std::ifstream verify(tmpPath, std::ios::binary);
    Check(verify.good(), "FileWrite with spaces should create file on disk");
    verify.close();
    DeleteFileA(tmpPath.c_str());
  }

  {
    agent::tools::ToolOrchestrator orchestrator;
    std::vector<agent::core::ContentBlock> blocks;
    blocks.push_back(agent::core::ContentBlock::MakeToolUse(
        "fw-edge-2", "FileWrite", R"({"file_path":"build\p3_empty.txt"})"));

    std::vector<agent::core::Message> messages;
    agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                         const std::vector<agent::core::Message>&) {
      agent::core::PermissionDecision d;
      d.behavior = agent::core::PermissionBehavior::Allow;
      return d;
    };
    auto result = orchestrator.Execute(blocks, canUse, messages);
    Check(result.errorCount > 0, "FileWrite with empty content should error");
  }

  {
    agent::tools::ToolOrchestrator orchestrator;
    std::vector<agent::core::ContentBlock> blocks;
    blocks.push_back(agent::core::ContentBlock::MakeToolUse(
        "fw-edge-3", "FileWrite", R"({"content":"missing path"})"));

    std::vector<agent::core::Message> messages;
    agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                         const std::vector<agent::core::Message>&) {
      agent::core::PermissionDecision d;
      d.behavior = agent::core::PermissionBehavior::Allow;
      return d;
    };
    auto result = orchestrator.Execute(blocks, canUse, messages);
    Check(result.errorCount > 0, "FileWrite with no path should error");
  }
}

void TestRealFileWriteBinarySafeContent() {
  std::string tmpPath = "build\\p3_binary_test.txt";
  std::string content;
  content += "Line with special chars: \t\r\n";
  content += "Backslash: \\ and quote: \" end.\n";
  content += "Unicode: \xC3\xA9 \xE2\x82\xAC\n";
  for (int i = 1; i < 32; ++i) {
    if (i != '\n' && i != '\r' && i != '\t') {
      content.push_back(static_cast<char>(i));
    }
  }
  content += "\n";

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::ContentBlock> blocks;

  nlohmann::json inputJson;
  inputJson["file_path"] = tmpPath;
  inputJson["content"] = content;
  blocks.push_back(agent::core::ContentBlock::MakeToolUse(
      "fw-bin-1", "FileWrite", inputJson.dump()));

  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                       const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };
  auto result = orchestrator.Execute(blocks, canUse, messages);
  Check(result.errorCount == 0, "FileWrite binary-safe content should not error");

  std::ifstream verify(tmpPath, std::ios::binary);
  Check(verify.good(), "FileWrite binary-safe file should exist on disk");
  std::vector<char> actualBytes((std::istreambuf_iterator<char>(verify)),
                                 std::istreambuf_iterator<char>());
  verify.close();
  Check(actualBytes.size() == content.size(),
        "FileWrite binary-safe byte count should match");

  bool byteMatch = (actualBytes.size() == content.size());
  for (std::size_t i = 0; i < actualBytes.size() && i < content.size(); ++i) {
    if (actualBytes[i] != content[i]) { byteMatch = false; break; }
  }
  Check(byteMatch, "FileWrite binary-safe content should match byte-for-byte");

  DeleteFileA(tmpPath.c_str());
}

void TestReadWriteAliases() {
  const std::string tmpPath = "build\\p3_write_alias_test.txt";
  const std::string content = "Alias write content.\n";

  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::Message> messages;
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json writeJson;
  writeJson["file_path"] = tmpPath;
  writeJson["content"] = content;
  auto writeResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "write-alias-1", "Write", writeJson.dump())},
      canUse, messages);
  Check(writeResult.errorCount == 0, "Write alias should not error");

  nlohmann::json readJson;
  readJson["file_path"] = tmpPath;
  auto readResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "read-alias-1", "Read", readJson.dump())},
      canUse, messages);
  Check(readResult.errorCount == 0, "Read alias should not error");

  bool foundContent = false;
  for (const auto& msg : readResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find(content) != std::string::npos) {
        foundContent = true;
      }
    }
  }
  Check(foundContent, "Read alias should return written content");

  DeleteFileA(tmpPath.c_str());
}

void TestWorkspaceRelativeWriteUsesTrustedRoot() {
  const std::string workspaceRoot = "build\\workspace-mode-root";
  const std::string relativePath = "project\\generated.txt";
  const std::string expectedPath = workspaceRoot + "\\project\\generated.txt";
  const std::string accidentalPath = relativePath;
  const std::string content = "generated inside trusted workspace\n";

  CreateDirectoryA(workspaceRoot.c_str(), nullptr);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  nlohmann::json payload;
  payload["file_path"] = relativePath;
  payload["content"] = content;

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  auto result = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "workspace-write-1", "FileWrite", payload.dump())},
      canUse, {});
  Check(result.errorCount == 0,
        "Workspace relative write should succeed");

  std::ifstream verify(expectedPath, std::ios::binary);
  Check(verify.good(),
        "Workspace relative write should land inside trusted root");
  std::string actual((std::istreambuf_iterator<char>(verify)),
                     std::istreambuf_iterator<char>());
  verify.close();
  Check(actual == content,
        "Workspace relative write content should match");

  DWORD outsideAttrs = GetFileAttributesA(accidentalPath.c_str());
  Check(outsideAttrs == INVALID_FILE_ATTRIBUTES,
        "Workspace relative write should not create sibling path outside workspace");

  DeleteFileA(expectedPath.c_str());
}

void TestWorkspaceAllowsAbsoluteExternalRead() {
  const std::string workspaceRoot = "build\\workspace-read-root";
  const std::string externalRelative = "build\\workspace-external-read.txt";
  const std::string externalAbsolute = FullPathOf(externalRelative);
  const std::string content = "outside workspace reference\n";

  CreateDirectoryA(workspaceRoot.c_str(), nullptr);
  {
    std::ofstream out(externalRelative, std::ios::binary | std::ios::trunc);
    out << content;
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  nlohmann::json payload;
  payload["file_path"] = externalAbsolute;

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  auto result = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "workspace-read-1", "FileRead", payload.dump())},
      canUse, {});
  Check(result.errorCount == 0,
        "Workspace absolute external read should succeed");

  bool foundContent = false;
  for (const auto& msg : result.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find(content) != std::string::npos) {
        foundContent = true;
      }
    }
  }
  Check(foundContent,
        "Workspace absolute external read should return external file content");

  DeleteFileA(externalRelative.c_str());
}

void TestWorkspaceRejectsEscapingWrite() {
  const std::string workspaceRoot = "build\\workspace-guard-root";
  const std::string escapedRelativePath = "..\\workspace-escape.txt";
  const std::string escapedTarget =
      FullPathOf("build\\workspace-guard-root\\..\\workspace-escape.txt");

  CreateDirectoryA(workspaceRoot.c_str(), nullptr);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  nlohmann::json payload;
  payload["file_path"] = escapedRelativePath;
  payload["content"] = "should be rejected";

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  auto result = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "workspace-write-escape", "FileWrite", payload.dump())},
      canUse, {});
  Check(result.errorCount > 0,
        "Workspace escaping write should be rejected");

  DWORD attrs = GetFileAttributesA(escapedTarget.c_str());
  Check(attrs == INVALID_FILE_ATTRIBUTES,
        "Workspace escaping write should not create file outside workspace");
}

void TestUnicodeWorkspaceReadWrite() {
  const std::string workspaceRoot = "build\\unicode-tool-root";
  const std::string relativePath = "中文目录\\结果.txt";
  const std::string expectedPath = workspaceRoot + "\\中文目录\\结果.txt";
  const std::string content = "unicode payload\n第二行\n";

  CreateDirectoryA(workspaceRoot.c_str(), nullptr);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json writeJson;
  writeJson["file_path"] = relativePath;
  writeJson["content"] = content;
  auto writeResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "unicode-write-1", "FileWrite", writeJson.dump())},
      canUse, {});
  Check(writeResult.errorCount == 0,
        "Unicode workspace write should succeed");

  DWORD attrs = GetFileAttributesW(Utf8ToWide(expectedPath).c_str());
  Check(attrs != INVALID_FILE_ATTRIBUTES,
        "Unicode workspace write should create the target file");

  nlohmann::json readJson;
  readJson["file_path"] = relativePath;
  auto readResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "unicode-read-1", "FileRead", readJson.dump())},
      canUse, {});
  Check(readResult.errorCount == 0,
        "Unicode workspace read should succeed");

  bool foundContent = false;
  for (const auto& msg : readResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find(content) != std::string::npos) {
        foundContent = true;
      }
    }
  }
  Check(foundContent,
        "Unicode workspace read should return the written content");

  DeleteFileW(Utf8ToWide(expectedPath).c_str());
}

void TestTuiTaskPanelLoadsAndPrioritizesTasks() {
  const std::string taskStorePath = "build\\tui-task-panel.json";
  {
    std::ofstream out(taskStorePath, std::ios::binary | std::ios::trunc);
    out << R"([
      {"id":"3","subject":"done task","status":"completed","owner":"bot"},
      {"id":"2","subject":"blocked task","status":"pending","blockedBy":["1"]},
      {"id":"1","subject":"active task","status":"in_progress","owner":"coder"},
      {"id":"4","subject":"open task","status":"pending","blockedBy":[]}
    ])";
  }

  const agent::app::TuiTaskPanelData data =
      agent::app::LoadTuiTaskPanelData(taskStorePath);
  Check(data.tasks.size() == 4, "Task panel should load all tasks");
  Check(data.inProgressCount == 1, "Task panel should count in-progress tasks");
  Check(data.pendingCount == 2, "Task panel should count pending tasks");
  Check(data.completedCount == 1, "Task panel should count completed tasks");
  Check(!data.tasks.empty() && data.tasks.front().id == "1",
        "Task panel should prioritize in-progress tasks first");

  const auto lines = agent::app::BuildTuiTaskPanelLines(data, 80, 3);
  Check(!lines.empty(), "Task panel should render summary lines");
  Check(lines.size() >= 2, "Task panel should render task rows");
}

void TestTaskToolsLifecycle() {
  const std::string workspaceRoot = "build\\task-tool-root";
  CreateDirectoryA(workspaceRoot.c_str(), nullptr);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json createJson;
  createJson["subject"] = "Implement tests";
  createJson["description"] = "Write targeted unit tests";
  auto createResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "task-create-1", "TaskCreate", createJson.dump())},
      canUse, {});
  Check(createResult.errorCount == 0, "TaskCreate should not error");

  auto listResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "task-list-1", "TaskList", "{}")},
      canUse, {});
  Check(listResult.errorCount == 0, "TaskList should not error");
  bool sawCreatedTask = false;
  for (const auto& msg : listResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find("Implement tests") != std::string::npos) {
        sawCreatedTask = true;
      }
    }
  }
  Check(sawCreatedTask, "TaskList should include created task");

  nlohmann::json updateJson;
  updateJson["id"] = "1";
  updateJson["status"] = "completed";
  auto updateResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "task-update-1", "TaskUpdate", updateJson.dump())},
      canUse, {});
  Check(updateResult.errorCount == 0, "TaskUpdate should not error");

  nlohmann::json getJson;
  getJson["id"] = "1";
  auto getResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "task-get-1", "TaskGet", getJson.dump())},
      canUse, {});
  bool sawCompleted = false;
  for (const auto& msg : getResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult &&
          block.asToolResult.content.find("\"completed\"") != std::string::npos) {
        sawCompleted = true;
      }
    }
  }
  Check(sawCompleted, "TaskGet should show updated status");

  nlohmann::json stopJson;
  stopJson["id"] = "1";
  auto stopResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "task-stop-1", "TaskStop", stopJson.dump())},
      canUse, {});
  Check(stopResult.errorCount == 0, "TaskStop should not error");
}

void TestNotebookEditLifecycle() {
  const std::string workspaceRoot = "build\\notebook-tool-root";
  const std::string notebookPath = workspaceRoot + "\\test.ipynb";
  CreateDirectoryA(workspaceRoot.c_str(), nullptr);

  nlohmann::json notebook;
  notebook["cells"] = nlohmann::json::array({
      {{"id", "cell-1"}, {"cell_type", "markdown"},
       {"metadata", nlohmann::json::object()},
       {"source", nlohmann::json::array({"hello"})}}
  });
  notebook["metadata"] = nlohmann::json::object();
  notebook["nbformat"] = 4;
  notebook["nbformat_minor"] = 5;
  {
    std::ofstream out(notebookPath, std::ios::binary | std::ios::trunc);
    out << notebook.dump(2);
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);
  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json replaceJson;
  replaceJson["notebook_path"] = "test.ipynb";
  replaceJson["cell_id"] = "cell-1";
  replaceJson["new_source"] = "updated";
  replaceJson["edit_mode"] = "replace";
  auto replaceResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "nb-replace-1", "NotebookEdit", replaceJson.dump())},
      canUse, {});
  Check(replaceResult.errorCount == 0, "NotebookEdit replace should not error");

  nlohmann::json insertJson;
  insertJson["notebook_path"] = "test.ipynb";
  insertJson["cell_id"] = "cell-1";
  insertJson["new_source"] = "print('ok')";
  insertJson["cell_type"] = "code";
  insertJson["edit_mode"] = "insert";
  auto insertResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "nb-insert-1", "NotebookEdit", insertJson.dump())},
      canUse, {});
  Check(insertResult.errorCount == 0, "NotebookEdit insert should not error");

  std::ifstream verify(notebookPath, std::ios::binary);
  nlohmann::json updated = nlohmann::json::parse(
      std::string((std::istreambuf_iterator<char>(verify)),
                  std::istreambuf_iterator<char>()));
  verify.close();
  Check(updated["cells"].size() == 2, "NotebookEdit insert should add a cell");
  Check(updated["cells"][0]["source"][0] == "updated",
        "NotebookEdit replace should update source");
}

void TestSkillToolDispatchesAgent() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::agents::SubAgentManager subAgentManager;
  orchestrator.SetSubAgentManager(&subAgentManager);

  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json skillJson;
  skillJson["command"] = "plan";
  skillJson["args"] = "Plan the implementation work";
  auto result = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "skill-1", "Skill", skillJson.dump())},
      canUse, {});
  Check(result.errorCount == 0, "Skill should dispatch via Agent tool");
  Check(!result.userMessages.empty(), "Skill should produce a tool result");
}

}  // namespace


// ===== P1-02 Tests: Bash mkdir normalization =====

void TestMkdirNormalization() {
  agent::tools::ToolOrchestrator orchestrator;
  auto canUse = [](const agent::core::ContentBlock&,
                   const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  {
    nlohmann::json bashJson;
    bashJson["command"] = "mkdir test-dir";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse(
            "mkdir-1", "Bash", bashJson.dump())},
        canUse, {});
    Check(!result.userMessages.empty(), "mkdir should produce result");
    bool sawNormalized = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("[normalized command] New-Item -ItemType Directory") !=
            std::string::npos) {
          sawNormalized = true;
        }
      }
    }
    Check(sawNormalized, "mkdir should normalize to New-Item -ItemType Directory");
  }

  {
    nlohmann::json bashJson;
    bashJson["command"] = "mkdir -p nested/test-dir";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse(
            "mkdir-p-1", "Bash", bashJson.dump())},
        canUse, {});
    bool sawForce = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("New-Item -ItemType Directory -Force") !=
            std::string::npos) {
          sawForce = true;
        }
      }
    }
    Check(sawForce, "mkdir -p should normalize to New-Item with -Force");
  }

  {
    nlohmann::json bashJson;
    bashJson["command"] = "mkdir \"space dir\"";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse(
            "mkdir-space-1", "Bash", bashJson.dump())},
        canUse, {});
    bool sawQuotedPath = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("-Path 'space dir'") !=
            std::string::npos) {
          sawQuotedPath = true;
        }
      }
    }
    Check(sawQuotedPath,
          "mkdir should preserve quoted paths with spaces as one argument");
  }

  {
    nlohmann::json bashJson;
    bashJson["command"] = "mkdir data/{a,b}";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse(
            "mkdir-brace-1", "Bash", bashJson.dump())},
        canUse, {});
    bool sawExpandedA = false;
    bool sawExpandedB = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("-Path 'data/a'") !=
            std::string::npos) {
          sawExpandedA = true;
        }
        if (block.asToolResult.content.find("-Path 'data/b'") !=
            std::string::npos) {
          sawExpandedB = true;
        }
      }
    }
    Check(sawExpandedA && sawExpandedB,
          "mkdir should keep brace expansion for unquoted paths");
  }

  {
    nlohmann::json bashJson;
    bashJson["command"] = "mkdir \"literal {a,b}\"";
    auto result = orchestrator.Execute(
        {agent::core::ContentBlock::MakeToolUse(
            "mkdir-literal-brace-1", "Bash", bashJson.dump())},
        canUse, {});
    bool sawLiteralBracePath = false;
    bool sawExpandedBracePath = false;
    for (const auto& msg : result.userMessages) {
      for (const auto& block : msg.content) {
        if (block.type != agent::core::BlockType::ToolResult) continue;
        if (block.asToolResult.content.find("-Path 'literal {a,b}'") !=
            std::string::npos) {
          sawLiteralBracePath = true;
        }
        if (block.asToolResult.content.find("-Path 'literal a'") !=
                std::string::npos ||
            block.asToolResult.content.find("-Path 'literal b'") !=
                std::string::npos) {
          sawExpandedBracePath = true;
        }
      }
    }
    Check(sawLiteralBracePath && !sawExpandedBracePath,
          "mkdir should not expand braces inside quoted literal paths");
  }

  RemoveDirectoryA("test-dir");
  RemoveDirectoryA("nested\test-dir");
  RemoveDirectoryA("nested");
  RemoveDirectoryA("space dir");
  RemoveDirectoryA("data\\a");
  RemoveDirectoryA("data\\b");
  RemoveDirectoryA("data");
  RemoveDirectoryA("literal {a,b}");
}

void TestGlobSupportsRecursivePatterns() {
  const std::string workspaceRoot = FullPathOf("build\\glob_recursive_test");
  CreateDirectoryA("build\\glob_recursive_test", nullptr);
  CreateDirectoryA("build\\glob_recursive_test\\src", nullptr);
  CreateDirectoryA("build\\glob_recursive_test\\src\\app", nullptr);
  CreateDirectoryA("build\\glob_recursive_test\\src\\core", nullptr);
  {
    std::ofstream out("build\\glob_recursive_test\\src\\app\\main.cpp",
                      std::ios::binary);
    out << "int main() { return 0; }\n";
  }
  {
    std::ofstream out("build\\glob_recursive_test\\src\\core\\QueryLoop.cpp",
                      std::ios::binary);
    out << "void QueryLoop() {}\n";
  }
  {
    std::ofstream out("build\\glob_recursive_test\\src\\README.md",
                      std::ios::binary);
    out << "# sample\n";
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetWorkspaceRoot(workspaceRoot);
  agent::core::CanUseTool canUse = [](const agent::core::ContentBlock&,
                                      const std::vector<agent::core::Message>&) {
    agent::core::PermissionDecision d;
    d.behavior = agent::core::PermissionBehavior::Allow;
    return d;
  };

  nlohmann::json recursiveGlob;
  recursiveGlob["pattern"] = "src/**/*.cpp";
  auto recursiveResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "glob-recursive-1", "Glob", recursiveGlob.dump())},
      canUse, {});
  bool sawAppCpp = false;
  bool sawCoreCpp = false;
  for (const auto& msg : recursiveResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.content.find("app/main.cpp") != std::string::npos) {
        sawAppCpp = true;
      }
      if (block.asToolResult.content.find("core/QueryLoop.cpp") !=
          std::string::npos) {
        sawCoreCpp = true;
      }
    }
  }
  Check(sawAppCpp, "Glob should match recursive cpp file under app");
  Check(sawCoreCpp, "Glob should match recursive cpp file under core");

  nlohmann::json directChildrenGlob;
  directChildrenGlob["pattern"] = "src/*";
  auto directChildrenResult = orchestrator.Execute(
      {agent::core::ContentBlock::MakeToolUse(
          "glob-recursive-2", "Glob", directChildrenGlob.dump())},
      canUse, {});
  bool sawAppDir = false;
  bool sawNestedCppAsDirectChild = false;
  for (const auto& msg : directChildrenResult.userMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.content.find("[DIR]  app") != std::string::npos) {
        sawAppDir = true;
      }
      if (block.asToolResult.content.find("app/main.cpp") != std::string::npos) {
        sawNestedCppAsDirectChild = true;
      }
    }
  }
  Check(sawAppDir, "Glob should still return direct child directories");
  Check(!sawNestedCppAsDirectChild,
        "Glob direct child pattern should not include nested recursive files");

  DeleteFileA("build\\glob_recursive_test\\src\\app\\main.cpp");
  DeleteFileA("build\\glob_recursive_test\\src\\core\\QueryLoop.cpp");
  DeleteFileA("build\\glob_recursive_test\\src\\README.md");
  RemoveDirectoryA("build\\glob_recursive_test\\src\\app");
  RemoveDirectoryA("build\\glob_recursive_test\\src\\core");
  RemoveDirectoryA("build\\glob_recursive_test\\src");
  RemoveDirectoryA("build\\glob_recursive_test");
}

int main() {
  TestToolRegistry();
  TestToolPartition();
  TestRealBash();
  TestWindowsLsCompatibility();
  TestWindowsDirCompatibility();
  TestRealFileRead();
  TestFileReadSupportsOffsetLimit();
  TestLargeFileReadRequiresTargetedRange();
  TestDeniedExecution();
  TestAskExecutionBehavesAsDeniedInNonInteractiveMode();
  TestRealFileWrite();
  TestRealFileWriteOverwrite();
  TestRealFileWriteEdgeCases();
  TestRealFileWriteBinarySafeContent();
  TestReadWriteAliases();
  TestWorkspaceRelativeWriteUsesTrustedRoot();
  TestWorkspaceAllowsAbsoluteExternalRead();
  TestWorkspaceRejectsEscapingWrite();
  TestUnicodeWorkspaceReadWrite();
  TestTuiTaskPanelLoadsAndPrioritizesTasks();
  TestTaskToolsLifecycle();
  TestNotebookEditLifecycle();
  TestSkillToolDispatchesAgent();
  TestMkdirNormalization();
  TestGlobSupportsRecursivePatterns();
  std::cout << "[test_tools] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
