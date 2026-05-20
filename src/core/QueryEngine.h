#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <string>
#include <vector>

namespace agent {
namespace api {
class ModelClient;
class SideQueryClient;
}
namespace agents {
class SubAgentManager;
}
namespace memory {
class MemoryIndex;
}
namespace permissions {
class PermissionEngine;
}
namespace tools {
class ToolOrchestrator;
class ToolRegistry;
}
namespace infra {
class SessionManager;
class StabilityWatchdog;
}
namespace core {

struct ContentReplacementState {
  std::vector<std::string> seenIds;
  std::vector<std::string> replacementTexts;
  std::string lastSeenId;

  bool HasSeen(const std::string& toolUseId) const;
  std::string GetReplacement(const std::string& toolUseId) const;
  void RecordReplacement(const std::string& toolUseId,
                         const std::string& replacement);
};

struct AutoCompactTrackingState {
  bool compacted = false;
  int turnCounter = 0;
  std::string turnId;
  int consecutiveFailures = 0;
};

struct QueryLoopContext {
  std::vector<Message> messages;
  std::string systemPrompt;
  std::string model;
  std::string fallbackModel;
  std::string validatorModel;
  ContentReplacementState replacementState;
  AutoCompactTrackingState autoCompactTracking;
  int maxOutputTokensRecoveryCount = 0;
  bool hasAttemptedReactiveCompact = false;
  std::string sessionDir;
  infra::SessionManager* sessionManager = nullptr;
};

struct ValidationToolIntervention {
  std::string toolUseId;
  std::string action;
  std::string correctedName;
  std::string correctedInputJson;
  std::string blockGuidance;
};

struct ValidationResult {
  std::string correctedText;
  std::vector<ValidationToolIntervention> toolInterventions;
  std::string finalResponseAction;
  std::string retryGuidance;
};

class QueryEngine {
 public:
  QueryEngine(tools::ToolOrchestrator& toolOrchestrator,
              permissions::PermissionEngine& permissionEngine,
              api::ModelClient& modelClient,
              api::SideQueryClient& sideQueryClient,
              tools::ToolRegistry& toolRegistry,
              infra::SessionManager& sessionManager);

  void SetConfig(const AgentConfig& config);
  void SetSystemPrompt(const std::string& systemPrompt);
  void SetModel(const std::string& model);
  void SetFallbackModel(const std::string& model);
  void SetValidatorModel(const std::string& model);
  void SetMemoryIndex(memory::MemoryIndex* memoryIndex);
  void SetSubAgentManager(agents::SubAgentManager* subAgentManager);
  void SetStabilityWatchdog(infra::StabilityWatchdog* watchdog);
  void SetMaxTurns(int maxTurns);
  void SetSessionDir(const std::string& sessionDir);

  void SubmitUserPrompt(const std::string& prompt);
  void RunTurn();
  bool RunTurnWithRecovery();

  const std::vector<Message>& messages() const;
  const QueryLoopContext& loopContext() const;

 private:
  tools::ToolOrchestrator& toolOrchestrator_;
  permissions::PermissionEngine& permissionEngine_;
  api::ModelClient& modelClient_;
  api::SideQueryClient& sideQueryClient_;
  tools::ToolRegistry& toolRegistry_;
  infra::SessionManager& sessionManager_;
  memory::MemoryIndex* memoryIndex_ = nullptr;
  agents::SubAgentManager* subAgentManager_ = nullptr;
  infra::StabilityWatchdog* stabilityWatchdog_ = nullptr;

  AgentConfig config_;
  std::string systemPrompt_;
  std::string model_;
  std::string fallbackModel_;
  std::string validatorModel_;
  std::vector<Message> messages_;
  SessionMetadata metadata_;
  QueryLoopContext loopCtx_;
  int maxTurns_ = 500;
  std::string sessionDir_;

  std::string BuildEffectiveSystemPrompt() const;
  std::string BuildLatestUserQuery() const;
  void SyncSessionState();
  void ConfigureWatchdogBindings();
  bool RecoverFromSnapshot();
  void SaveCheckpoint();
  bool RunOneLoopIteration();
  bool HandleFallback();
};

}  // namespace core
}  // namespace agent
