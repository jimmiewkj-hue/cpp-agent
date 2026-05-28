#include "app/RuntimePolicy.h"
#include "agents/SubAgentManager.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "core/QueryEngine.h"
#include "infra/SessionManager.h"
#include "mcp/McpClientManager.h"
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

std::wstring Utf8ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int len = MultiByteToWideChar(
      CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                      &wide[0], len);
  return wide;
}

bool DirectoryExists(const std::string& path) {
  DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
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
  const std::wstring widePath = Utf8ToWide(path);
  return CreateDirectoryW(widePath.c_str(), nullptr) ||
         GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string ReadAllText(const std::string& path) {
  const std::wstring widePath = Utf8ToWide(path);
  HANDLE h = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::string();
  LARGE_INTEGER size;
  if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) {
    CloseHandle(h);
    return std::string();
  }
  std::string content(static_cast<std::size_t>(size.QuadPart), '\0');
  DWORD totalRead = 0;
  while (totalRead < content.size()) {
    DWORD chunk = 0;
    if (!ReadFile(h, &content[totalRead],
                  static_cast<DWORD>(content.size() - totalRead),
                  &chunk, nullptr)) {
      CloseHandle(h);
      return std::string();
    }
    if (chunk == 0) break;
    totalRead += chunk;
  }
  CloseHandle(h);
  content.resize(totalRead);
  return content;
}

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  char buffer[512] = {0};
  DWORD len = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  if (len == 0 || len >= sizeof(buffer)) return fallback;
  return std::string(buffer, len);
}

int GetEnvIntOrDefault(const char* name, int fallback) {
  const std::string value = GetEnvOrDefault(name, std::string());
  if (value.empty()) return fallback;
  char* end = nullptr;
  const long parsed = std::strtol(value.c_str(), &end, 10);
  if (end == value.c_str() || (end != nullptr && *end != '\0') || parsed <= 0) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

// #region debug-point A:runner-debug-env
void LoadDebugEnvIntoProcess() {
  char enabled[16] = {0};
  DWORD enabledLen = GetEnvironmentVariableA(
      "CPP_AGENT_ENABLE_DEBUG_SERVER", enabled, sizeof(enabled));
  const std::string enabledValue =
      enabledLen > 0 && enabledLen < sizeof(enabled)
          ? std::string(enabled, enabledLen)
          : std::string();
  if (enabledValue != "1" && enabledValue != "true" &&
      enabledValue != "TRUE") {
    return;
  }
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
  SetCurrentDirectoryW(Utf8ToWide(workspaceRoot).c_str());

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

  for (const auto& tool : agent::app::GetSessionBaseTools(false)) {
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
  config.systemPrompt =
      agent::app::BuildWorkspaceSystemPrompt(workspaceRoot, true);
  config.defaultModel = llmCfg.mainModel;
  config.memoryRoot = memoryDir;
  config.sessionDir = sessionDir;
  std::cout << "configured_prompt_has_workspace_first="
            << (config.systemPrompt.find(
                    "MUST first explore the workspace with Glob, Grep, or Read tools") !=
                std::string::npos
                    ? "true"
                    : "false")
            << std::endl;
  std::cout << "configured_prompt_has_powershell="
            << (config.systemPrompt.find("PowerShell, not bash") !=
                        std::string::npos
                    ? "true"
                    : "false")
            << std::endl;

  agent::core::QueryEngine engine(
      toolOrchestrator, permissionEngine, httpClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetConfig(config);
  engine.SetSystemPrompt(config.systemPrompt);
  engine.SetModel(llmCfg.mainModel);
  engine.SetFallbackModel(llmCfg.fallbackModel);
  engine.SetValidatorModel(llmCfg.validatorModel);
  engine.SetMemoryIndex(&memoryIndex);
  engine.SetSubAgentManager(&subAgentManager);
  engine.SetSessionDir(sessionDir);
  const int runnerMaxTurns =
      GetEnvIntOrDefault("CPP_AGENT_RUNNER_MAX_TURNS", 80);
  const int runnerWallClockBudgetMs = GetEnvIntOrDefault(
      "CPP_AGENT_RUNNER_WALL_CLOCK_BUDGET_MS", 10 * 60 * 1000);
  const int runnerMaxSegments =
      GetEnvIntOrDefault("CPP_AGENT_RUNNER_MAX_SEGMENTS", 4);
  engine.SetMaxTurns(runnerMaxTurns);
  engine.SetWallClockBudgetMs(runnerWallClockBudgetMs);
  std::cout << "configured_max_turns=" << runnerMaxTurns << std::endl;
  std::cout << "configured_wall_clock_budget_ms=" << runnerWallClockBudgetMs
            << std::endl;
  std::cout << "configured_max_segments=" << runnerMaxSegments << std::endl;
  int assistantEventCount = 0;
  int toolResultEventCount = 0;
  std::string lastTerminalReason;
  engine.SetEventCallback([&](const agent::core::QueryLoopEvent& event) {
    if (event.type == agent::core::QueryLoopEvent::Type::StageChanged) {
      std::cout << "stage=" << static_cast<int>(event.stage) << std::endl;
      return;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::ToolResult) {
      ++toolResultEventCount;
      return;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::AssistantMessage) {
      ++assistantEventCount;
      return;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::LoopCompleted) {
      lastTerminalReason = event.terminalReason;
      std::cout << "loop_completed_reason=" << event.terminalReason << std::endl;
    }
  });

  std::cout << "submitting_prompt=true" << std::endl;
  engine.SubmitUserPrompt(prompt);
  bool ok = true;
  int segmentCount = 0;
  int heartbeatBase = 0;
  while (segmentCount < runnerMaxSegments) {
    ++segmentCount;
    std::cout << "running_turn=true" << std::endl;
    std::cout << "running_turn_segment=" << segmentCount << std::endl;
    lastTerminalReason.clear();
    // #region debug-point E:runner-heartbeat
    std::atomic<bool> runCompleted(false);
    std::thread heartbeat([&]() {
      int beat = heartbeatBase;
      while (!runCompleted.load()) {
        Sleep(5000);
        if (runCompleted.load()) break;
        std::cout << "runner_heartbeat=" << (++beat) << std::endl;
      }
      heartbeatBase = beat;
    });
    // #endregion
    const bool segmentOk = engine.RunTurnWithRecovery();
    runCompleted.store(true);
    if (heartbeat.joinable()) heartbeat.join();
    ok = ok && segmentOk;
    if (!segmentOk) break;
    if (lastTerminalReason != "wall_clock_budget_exceeded") break;
    if (segmentCount >= runnerMaxSegments) {
      std::cout << "runner_segment_limit_reached=true" << std::endl;
      break;
    }
    const bool prepared = engine.PrepareForContinuationAfterWallClockTimeout();
    std::cout << "continuing_after_wall_clock="
              << (prepared ? "true" : "false") << std::endl;
    if (!prepared) break;
  }
  std::cout << "run_finished=true" << std::endl;
  sessionManager.FlushTranscriptBuffer();
  sessionManager.SetMessages(engine.messages());
  sessionManager.PersistSnapshot();
  std::cout << "effective_prompt_has_workspace_first="
            << (engine.loopContext().systemPrompt.find(
                    "MUST first explore the workspace with Glob, Grep, or Read tools") !=
                std::string::npos
                    ? "true"
                    : "false")
            << std::endl;
  std::cout << "assistant_event_count=" << assistantEventCount << std::endl;
  std::cout << "tool_result_event_count=" << toolResultEventCount << std::endl;

  const std::string snapshot = ReadAllText(sessionManager.LegacySnapshotPath());
  const std::string transcript = ReadAllText(sessionManager.TranscriptJsonlPath());
  const int turnCount = ExtractTurnCount(snapshot);

  std::cout << "run_ok=" << (ok ? "true" : "false") << "\n";
  std::cout << "workspace=" << workspaceRoot << "\n";
  std::cout << "session_dir=" << sessionDir << "\n";
  std::cout << "snapshot_path=" << sessionManager.LegacySnapshotPath() << "\n";
  std::cout << "snapshot_pb_path=" << sessionManager.SnapshotPath() << "\n";
  std::cout << "transcript_path=" << sessionManager.TranscriptJsonlPath() << "\n";
  std::cout << "turn_count=" << turnCount << "\n";
  std::cout << "message_count=" << engine.messages().size() << "\n";
  std::cout << "transcript_size=" << transcript.size() << "\n";
  return ok ? 0 : 1;
}
