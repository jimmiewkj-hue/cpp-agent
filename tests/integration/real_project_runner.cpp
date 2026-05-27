#include "agents/SubAgentManager.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "core/QueryEngine.h"
#include "infra/SessionManager.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <windows.h>

#include <cstdlib>
#include <cctype>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string JoinPath(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty()) return rhs;
  if (rhs.empty()) return lhs;
  const char last = lhs[lhs.size() - 1];
  if (last == '\\' || last == '/') return lhs + rhs;
  return lhs + "\\" + rhs;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesA(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES &&
         (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool EnsureDirectoryRecursive(const std::string& path) {
  if (path.empty()) return false;
  if (DirectoryExists(path)) return true;
  std::size_t pos = path.find_last_of("\\/");
  if (pos != std::string::npos) {
    const std::string parent = path.substr(0, pos);
    if (!parent.empty() && !DirectoryExists(parent)) {
      if (!EnsureDirectoryRecursive(parent)) return false;
    }
  }
  return CreateDirectoryA(path.c_str(), nullptr) ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string ReadAllText(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  char buffer[512] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return fallback;
  return std::string(buffer, len);
}

// #region debug-point A:runner-debug-env
void LoadDebugEnvIntoProcess() {
  std::ifstream in(".dbg\\stream-response-stall.env", std::ios::binary);
  if (!in) return;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string value = line.substr(eq + 1);
    if (!key.empty()) {
      SetEnvironmentVariableA(key.c_str(), value.c_str());
    }
  }
}
// #endregion

int ExtractTurnCount(const std::string& snapshot) {
  const std::string token = "turn_count=";
  const std::size_t start = snapshot.find(token);
  if (start == std::string::npos) return -1;
  const std::size_t valueStart = start + token.size();
  std::size_t valueEnd = valueStart;
  while (valueEnd < snapshot.size() &&
         std::isdigit(static_cast<unsigned char>(snapshot[valueEnd]))) {
    ++valueEnd;
  }
  if (valueEnd == valueStart) return -1;
  return std::atoi(snapshot.substr(valueStart, valueEnd - valueStart).c_str());
}

std::string BuildSystemPrompt(const std::string& workspaceRoot) {
  std::ostringstream prompt;
  prompt
      << "You are a helpful coding agent. Use the available tools to inspect "
      << "code, explain findings, and make careful changes when requested. "
      << "The trusted workspace root is `" << workspaceRoot << "`. "
      << "Treat relative file paths as paths inside this workspace. "
      << "If inspection, tests, or file changes are required, use tools to do "
      << "real work instead of only describing a plan.";
  return prompt.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::cerr << "Usage: agent_real_project_runner <workspace> <prompt-file> [session-dir]\n";
    return 2;
  }

  const std::string workspaceRoot = argv[1];
  const std::string promptFile = argv[2];
  const std::string sessionDir =
      argc >= 4 ? argv[3]
                : JoinPath(JoinPath(workspaceRoot, ".cpp-agent"),
                           "session_multiturn_regression");
  const std::string memoryDir =
      JoinPath(JoinPath(workspaceRoot, ".cpp-agent"), "memory_multiturn_regression");
  const std::string prompt = ReadAllText(promptFile);
  if (prompt.empty()) {
    std::cerr << "Prompt file is empty: " << promptFile << "\n";
    return 2;
  }

  LoadDebugEnvIntoProcess();

  std::cout << "workspace=" << workspaceRoot << std::endl;
  std::cout << "session_dir=" << sessionDir << std::endl;
  std::cout << "memory_dir=" << memoryDir << std::endl;

  if (!EnsureDirectoryRecursive(sessionDir)) {
    std::cerr << "Failed to create session directory: " << sessionDir << std::endl;
    return 2;
  }
  if (!EnsureDirectoryRecursive(memoryDir)) {
    std::cerr << "Failed to create memory directory: " << memoryDir << std::endl;
    return 2;
  }
  std::cout << "directories_ready=true" << std::endl;
  SetCurrentDirectoryA(workspaceRoot.c_str());

  agent::core::LlmConfig llmCfg;
  llmCfg.apiEndpoint = GetEnvOrDefault(
      "CPP_AGENT_API_ENDPOINT", "http://127.0.0.1:8080/v1/chat/completions");
  llmCfg.mainModel = GetEnvOrDefault(
      "CPP_AGENT_MAIN_MODEL", "Qwen3.6-35B-A3B-UD-Q6_K");
  llmCfg.validatorModel = GetEnvOrDefault(
      "CPP_AGENT_VALIDATOR_MODEL", "");
  llmCfg.fallbackModel = GetEnvOrDefault(
      "CPP_AGENT_FALLBACK_MODEL", "gemma-4-31B-it-Q8_0");
  llmCfg.connectTimeoutMs = 30000;
  llmCfg.requestTimeoutMs = 180000;

  agent::api::HttpLlmClient httpClient(llmCfg);
  agent::api::SideQueryClient sideQueryClient(httpClient);
  agent::agents::SubAgentManager subAgentManager;
  agent::mcp::McpClientManager mcpClientManager;
  agent::memory::MemoryIndex memoryIndex(memoryDir);
  agent::tools::ToolOrchestrator toolOrchestrator;
  agent::tools::ToolRegistry toolRegistry;
  agent::permissions::PermissionEngine permissionEngine;
  agent::infra::SessionManager sessionManager(sessionDir);

  for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
    toolRegistry.RegisterTool(tool);
    permissionEngine.AddAlwaysAllowRule(tool.name);
    permissionEngine.AddAutoModeAllowlistedTool(tool.name);
  }
  permissionEngine.SetPermissionMode(agent::core::PermissionMode::BypassPermissions);

  toolOrchestrator.SetToolRegistry(&toolRegistry);
  toolOrchestrator.SetSubAgentManager(&subAgentManager);
  toolOrchestrator.SetMcpClientManager(&mcpClientManager);
  toolOrchestrator.SetWorkspaceRoot(workspaceRoot);
  subAgentManager.SetMemoryIndex(&memoryIndex);

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt = BuildSystemPrompt(workspaceRoot);
  config.defaultModel = llmCfg.mainModel;
  config.memoryRoot = memoryDir;
  config.sessionDir = sessionDir;

  agent::core::QueryEngine engine(
      toolOrchestrator, permissionEngine, httpClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetConfig(config);
  engine.SetModel(llmCfg.mainModel);
  engine.SetFallbackModel(llmCfg.fallbackModel);
  engine.SetValidatorModel(llmCfg.validatorModel);
  engine.SetMemoryIndex(&memoryIndex);
  engine.SetSubAgentManager(&subAgentManager);
  engine.SetSessionDir(sessionDir);
  engine.SetMaxTurns(30);

  std::cout << "submitting_prompt=true" << std::endl;
  engine.SubmitUserPrompt(prompt);
  std::cout << "running_turn=true" << std::endl;
  // #region debug-point E:runner-heartbeat
  std::atomic<bool> runCompleted(false);
  std::thread heartbeat([&]() {
    int beat = 0;
    while (!runCompleted.load()) {
      Sleep(5000);
      if (runCompleted.load()) break;
      std::cout << "runner_heartbeat=" << (++beat) << std::endl;
    }
  });
  // #endregion
  const bool ok = engine.RunTurnWithRecovery();
  runCompleted.store(true);
  if (heartbeat.joinable()) heartbeat.join();
  std::cout << "run_finished=true" << std::endl;
  sessionManager.FlushTranscriptBuffer();

  const std::string snapshot = ReadAllText(sessionManager.SnapshotPath());
  const std::string transcript = ReadAllText(sessionManager.TranscriptJsonlPath());
  const int turnCount = ExtractTurnCount(snapshot);

  std::cout << "run_ok=" << (ok ? "true" : "false") << "\n";
  std::cout << "workspace=" << workspaceRoot << "\n";
  std::cout << "session_dir=" << sessionDir << "\n";
  std::cout << "snapshot_path=" << sessionManager.SnapshotPath() << "\n";
  std::cout << "transcript_path=" << sessionManager.TranscriptJsonlPath() << "\n";
  std::cout << "turn_count=" << turnCount << "\n";
  std::cout << "message_count=" << engine.messages().size() << "\n";
  std::cout << "transcript_size=" << transcript.size() << "\n";
  return ok ? 0 : 1;
}
