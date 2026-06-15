#pragma once

#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <functional>
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
namespace hooks {
class HookExecutor;
}
namespace core {

using QueryLoopEventCallback = std::function<void(const QueryLoopEvent&)>;

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
  // Aligned with local-ace's querySource: identifies the caller context
  // (e.g. "repl_main_thread", "compact", "session_memory", "auto_dream").
  // Used to prevent recursive compaction and model-call loops.
  std::string querySource;
  ContentReplacementState replacementState;
  AutoCompactTrackingState autoCompactTracking;
  int maxOutputTokensRecoveryCount = 0;
  bool hasAttemptedReactiveCompact = false;
  std::string sessionDir;
  infra::SessionManager* sessionManager = nullptr;
  std::string lastTerminalReason;
  QueryLoopEventCallback eventCallback;
  hooks::HookExecutor* hookExecutor = nullptr;
};

struct ValidationToolIntervention {
  std::string toolUseId;
  std::string action;
  std::string correctedName;
  std::string correctedInputJson;
  std::string blockGuidance;
};

// STRENGTHEN-T08: a structured, non-destructive correction suggestion.
// Unlike correctedText (which hard-replaces the assistant's voice and
// breaks tonal continuity), patches are injected as advisory review notes
// the main model decides whether to adopt on the next turn.
struct ValidationPatch {
  std::string anchor;            // location/quote the patch refers to
  std::string issue;             // what's wrong
  std::string suggestedReplace;  // suggested replacement text
};

struct ValidationResult {
  std::string correctedText;
  std::vector<ValidationToolIntervention> toolInterventions;
  std::string finalResponseAction;
  std::string retryGuidance;
  // STRENGTHEN-T08: structured patches (preferred over correctedText)
  std::vector<ValidationPatch> patches;
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
  void SetWallClockBudgetMs(long long budgetMs);
  void SetSessionDir(const std::string& sessionDir);
  void SetEventCallback(QueryLoopEventCallback callback);
  void SetHookExecutor(hooks::HookExecutor* hookExecutor);

  void SubmitUserPrompt(const std::string& prompt);
  void RunTurn();
  bool RunTurnWithRecovery();
  bool PrepareForContinuationAfterWallClockTimeout();

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
  int maxTurns_ = 0;
  long long wallClockBudgetMs_ = 0;
  std::string sessionDir_;
  QueryLoopEventCallback eventCallback_;
  hooks::HookExecutor* hookExecutor_ = nullptr;

  std::string BuildEffectiveSystemPrompt() const;
  std::string BuildLatestUserQuery() const;
  void SyncSessionState();
  void ConfigureWatchdogBindings();
  bool RecoverFromSnapshot();
  void SaveCheckpoint();
  bool RunOneLoopIteration();
  // STRENGTHEN-T05: HandleFallback() removed. Fallback is now handled
  // inside the main loop (QueryLoop.cpp ModelCall stage swaps
  // state.activeModel + ctx.model on api_error). This loop-external
  // method was dead code (never called) and is deleted to avoid confusion.
};

}  // namespace core
}  // namespace agent
