#include "core/QueryEngine.h"
#include "core/StateTypes.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "agents/SubAgentManager.h"
#include "infra/SessionManager.h"
#include "infra/StabilityWatchdog.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <iostream>
#include <string>

int main() {
  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  llmCfg.mainModel = "Qwen3.6-35B-A3B-UD-Q6_K";
  llmCfg.validatorModel = "gemma-4-31B-it-Q8_0";
  llmCfg.fallbackModel = "gemma-4-31B-it-Q8_0";
  llmCfg.connectTimeoutMs = 30000;
  llmCfg.requestTimeoutMs = 120000;

  agent::api::HttpLlmClient httpClient(llmCfg);
  agent::api::SideQueryClient sideQueryClient(httpClient);
  agent::agents::SubAgentManager subAgentManager;
  agent::memory::MemoryIndex memoryIndex(
      "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\session-memory");
  agent::tools::ToolOrchestrator toolOrchestrator;
  agent::tools::ToolRegistry toolRegistry;
  agent::permissions::PermissionEngine permissionEngine;
  agent::infra::SessionManager sessionManager(
      "G:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent\\build\\session");
  agent::infra::StabilityWatchdog watchdog(
      agent::infra::StabilityConfig{});

  toolRegistry.RegisterTool(
      {"FileRead", "Read files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});
  toolRegistry.RegisterTool(
      {"Grep", "Search files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});
  toolRegistry.RegisterTool(
      {"Glob", "Find files", R"({"type":"object"})",
       agent::tools::ToolExecCategory::ReadOnly, true, false, 100000});

  permissionEngine.AddAlwaysAllowRule("FileRead");

  toolOrchestrator.SetToolRegistry(&toolRegistry);

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt = "You are a helpful coding agent. You can read files, search code, and provide technical guidance.";
  config.defaultModel = llmCfg.mainModel;

  agent::core::QueryEngine engine(
      toolOrchestrator, permissionEngine, httpClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetConfig(config);
  engine.SetModel(llmCfg.mainModel);
  engine.SetFallbackModel(llmCfg.fallbackModel);
  engine.SetMemoryIndex(&memoryIndex);
  engine.SetSubAgentManager(&subAgentManager);

  watchdog.Start();
  engine.SetStabilityWatchdog(&watchdog);

  std::cout << "=== cpp-agent running with real llama.cpp models ===" << std::endl;
  std::cout << "main model:    " << llmCfg.mainModel << std::endl;
  std::cout << "validator:     " << llmCfg.validatorModel << std::endl;
  std::cout << "endpoint:      " << llmCfg.apiEndpoint << std::endl;
  std::cout << "native/oaicmp: " << (agent::api::HttpLlmClient::IsNativeAnthropicEndpoint(llmCfg.apiEndpoint) ? "Anthropic" : "OpenAI-compatible") << std::endl;
  std::cout << std::endl;

  engine.SubmitUserPrompt("Bootstrap the C++ agent from local-ace logic.");
  engine.RunTurn();

  const auto& msgs = engine.messages();

  std::cout << "=== bootstrap done ===" << std::endl;
  std::cout << "total_messages=" << msgs.size() << std::endl;
  std::cout << "total_turns=" << watchdog.metrics().totalTurns << std::endl;
  std::cout << "watchdog_healthy=" << watchdog.IsHealthy() << std::endl;

  for (const auto& m : msgs) {
    std::cout << "  role=";
    switch (m.role) {
      case agent::core::MessageRole::User:    std::cout << "user"; break;
      case agent::core::MessageRole::Assistant: std::cout << "asst"; break;
      case agent::core::MessageRole::System:  std::cout << "sys"; break;
    }
    std::cout << " uuid=" << m.uuid;
    std::cout << " blocks=" << m.content.size();
    if (m.isApiErrorMessage) std::cout << " IS_API_ERROR";
    if (m.isMeta) std::cout << " IS_META";
    std::cout << std::endl;
    for (std::size_t i = 0; i < m.content.size(); ++i) {
      const auto& b = m.content[i];
      std::cout << "    [" << i << "] type=";
      switch (b.type) {
        case agent::core::BlockType::Text:
          std::cout << "text len=" << b.asText.text.size();
          if (b.asText.text.size() > 300)
            std::cout << " preview=\"" << b.asText.text.substr(0, 300) << "...\"";
          else
            std::cout << " text=\"" << b.asText.text << "\"";
          break;
        case agent::core::BlockType::ToolUse:
          std::cout << "tool_use name=" << b.asToolUse.name
                    << " id=" << b.asToolUse.id;
          break;
        case agent::core::BlockType::ToolResult:
          std::cout << "tool_result id=" << b.asToolResult.toolUseId
                    << " isError=" << b.asToolResult.isError;
          break;
      }
      std::cout << std::endl;
    }
  }

  sessionManager.PersistSnapshot();
  watchdog.Stop();

  return 0;
}
