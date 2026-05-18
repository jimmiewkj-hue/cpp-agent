#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace agent {
namespace core {

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
  std::string systemPrompt = "You are a helpful coding agent.";
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
};

}  // namespace core
}  // namespace agent
