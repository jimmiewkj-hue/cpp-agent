#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace agent {
namespace core {

enum class PermissionMode {
  Default,
  Auto,
  BypassPermissions,
  Plan,
  AcceptEdits,
  DontAsk
};

struct LlmConfig {
  std::string apiEndpoint;
  std::string apiKey;
  std::string mainModel;
  std::string validatorModel;
  std::string fallbackModel;
  int connectTimeoutMs = 30000;
  int requestTimeoutMs = 120000;
};

struct DenialTrackingState {
  int consecutive = 0;
  int total = 0;
  int maxConsecutive = 3;
  int maxTotal = 20;

  bool IsCircuitBroken() const {
    return consecutive >= maxConsecutive || total >= maxTotal;
  }

  void RecordDenial() {
    ++consecutive;
    ++total;
  }

  void RecordApproval() { consecutive = 0; }

  void Reset() {
    consecutive = 0;
    total = 0;
  }
};

struct AgentConfig {
  std::string memoryRoot;
  std::string sessionDir;
  std::string logDir;
  std::string defaultModel = "default-model";
  std::string systemPrompt =
      "You are a coding agent running inside a local project workspace. "
      "Use the available tools to inspect files, create files, and modify the "
      "workspace when the user's request implies real project changes. "
      "Prefer Read/Write-style tool calls over pasting large code blobs into "
      "chat when the result should exist as a real file on disk. "
      "Do not reveal chain-of-thought or write 'thinking process' in the "
      "final answer. Be concise, action-oriented, and continue the turn after "
      "tool results until the requested file or change is actually completed.";
  int maxToolUseConcurrency = 10;
  int perMessageBudgetLimit = 600000;
  int contextWindow = 200000;
  int autocompactBufferTokens = 13000;

  bool autoCompactEnabled = true;
  bool reactiveCompactEnabled = true;
  bool contextCollapseEnabled = true;
  bool historySnipEnabled = true;
  bool cachedMicrocompactEnabled = true;
  bool validatorEnabled = false;
  bool streamingToolExecutionEnabled = true;
  bool failClosedGate = true;
  bool autoModeEnabled = false;
  PermissionMode permissionMode = PermissionMode::Default;

  static AgentConfig FromDefaults();
};

struct AbortError {
  std::string message;
};

struct FallbackTriggeredError {
  std::string message;
};

struct SessionMetadata {
  std::string id;
  std::string startTime;
  int turnCount = 0;
  bool aborted = false;
  std::string lastTerminalReason;
};

}  // namespace core
}  // namespace agent
