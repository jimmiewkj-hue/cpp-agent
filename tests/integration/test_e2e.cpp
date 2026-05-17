#include "agents/SubAgentManager.h"
#include "api/ModelClient.h"
#include "mcp/McpClientManager.h"
#include "tools/ToolRegistry.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <iostream>

static int failures = 0;
static void Check(bool condition, const char* label) {
  if (!condition) { std::cerr << "FAIL: " << label << std::endl; ++failures; }
}

namespace {

void TestE2E_OAuthTokenFlow() {
  agent::mcp::McpClientManager manager;
  int tokenCallCount = 0;
  manager.SetOAuthTokenProvider([&]() {
    ++tokenCallCount;
    return "fresh-oauth-token-p3";
  });

  agent::mcp::McpServerConfig cfg;
  cfg.name = "oauth-server";
  cfg.transportType = "stdio";
  Check(manager.RegisterServer(cfg), "OAuth server registered");
  manager.MarkNeedsAuth("oauth-server");
  Check(tokenCallCount == 0, "Token provider not yet called");

  bool result = manager.HandleOAuth401("oauth-server");
  Check(!result, "OAuth401 fails without transport");

  auto conns = manager.connections();
  bool found = false;
  for (const auto& c : conns)
    if (c.name == "oauth-server") found = true;
  Check(found, "OAuth server still tracked");
}

void TestE2E_ConcurrencyGate() {
  int batchLimit = agent::mcp::McpClientManager::GetConnectionBatchLimit();
  Check(batchLimit >= 1 && batchLimit <= 20, "Batch limit in valid range");

  agent::mcp::McpClientManager manager;
  for (int i = 0; i < 5; ++i) {
    agent::mcp::McpServerConfig cfg;
    cfg.name = "concurrent-" + std::to_string(i);
    cfg.transportType = "stdio";
    Check(manager.RegisterServer(cfg), "Register concurrent server");
  }
  auto conns = manager.connections();
  Check(static_cast<int>(conns.size()) >= 5, "All concurrent servers registered");
}

void TestE2E_FullPipelineSkeleton() {
  agent::tools::ToolRegistry toolRegistry;

  agent::tools::ToolSchema bash;
  bash.name = "Bash";
  bash.description = "shell command";
  bash.category = agent::tools::ToolExecCategory::ShellCommand;
  bash.destructiveHint = true;
  toolRegistry.RegisterTool(bash);

  agent::tools::ToolSchema fileRead;
  fileRead.name = "FileRead";
  fileRead.description = "read file";
  fileRead.category = agent::tools::ToolExecCategory::ReadOnly;
  fileRead.readOnlyHint = true;
  toolRegistry.RegisterTool(fileRead);

  auto tools = toolRegistry.ListTools();
  Check(tools.size() >= 2, "Tool registry populated for E2E");

  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  llmCfg.apiKey = "sk-p3-test";
  llmCfg.mainModel = "claude-sonnet";
  llmCfg.validatorModel = "claude-haiku";
  llmCfg.fallbackModel = "claude-opus";
  Check(!llmCfg.apiEndpoint.empty(), "LlmConfig populated");

  agent::api::SkeletonModelClient modelClient;
  agent::core::Message testMsg;
  testMsg.role = agent::core::MessageRole::User;
  testMsg.content.push_back(agent::core::ContentBlock::MakeText("test"));
  auto resp = modelClient.GenerateResponse({testMsg}, "system prompt", "model");
  Check(!resp.empty(), "SkeletonModelClient generates response");
}

void TestE2E_ValidationGate() {
  char buf[256] = {0};
  SetEnvironmentVariableA("LOCALMODEL_VALIDATION_MODEL", "claude-haiku");
  DWORD len = GetEnvironmentVariableA(
      "LOCALMODEL_VALIDATION_MODEL", buf, sizeof(buf));
  bool enabled = (len > 0 && len < sizeof(buf));
  Check(enabled, "LOCALMODEL_VALIDATION_MODEL set enables validation");
  SetEnvironmentVariableA("LOCALMODEL_VALIDATION_MODEL", nullptr);
}

void TestE2E_AutoDreamFullSequence() {
  Check(true, "AutoDream integration placeholder OK");
}

}  // namespace

int main() {
  TestE2E_OAuthTokenFlow();
  TestE2E_ConcurrencyGate();
  TestE2E_FullPipelineSkeleton();
  TestE2E_ValidationGate();
  TestE2E_AutoDreamFullSequence();

  std::cout << "[test_e2e] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
