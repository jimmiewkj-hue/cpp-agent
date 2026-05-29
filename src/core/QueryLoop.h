#pragma once

#include "core/AgentTypes.h"
#include "core/QueryEngine.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace permissions { class PermissionEngine; }
namespace tools { class ToolOrchestrator; }
namespace api { class ModelClient; class SideQueryClient; }

namespace core {

enum class TransitionReason {
  None,
  CollapseDrainRetry,
  ReactiveCompactRetry,
  MaxOutputTokensEscalate,
  MaxOutputTokensRecovery,
  StopHookBlocking,
  TokenBudgetContinuation,
  ValidatorRetry,
  ForcedContinuation,
  ToolResultContinuation,
};

struct QueryLoopInternalState {
  QueryStage stage = QueryStage::ToolResultBudget;
  std::vector<Message> messagesForTurn;
  std::vector<Message> assistantMessages;
  std::vector<Message> toolResultMessages;
  std::vector<Message> pendingFollowupMessages;
  std::vector<ContentBlock> toolUseBlocks;
  bool completed = false;
  bool validatorRequestedRetry = false;
  int missingToolUsePromptCount = 0;
  bool hasPromptedForWorkspaceExploration = false;
  bool forceContinuation = false;
  bool stopHookActive = false;
  std::string terminalReason;
  std::string forceContinuationReason;
  std::string activeModel;
  int modelCallCount = 0;
  int forcedContinuationCount = 0;
  int turnCount = 0;
  int nextTurnCount = 0;
  int consecutiveAutoCompactFailures = 0;
  int maxOutputTokensRecoveryCount = 0;
  bool hasAttemptedReactiveCompact = false;
  bool hasAttemptedCollapseDrain = false;
  int maxOutputTokensOverride = 0;
  TransitionReason transition = TransitionReason::None;
  // P0-02: Wall-clock budget (ms), 0 = unlimited
  long long wallClockBudgetMs = 0;
  // P0-02: Duplicate tool call detection
  std::vector<std::string> recentToolFingerprints;
  int consecutiveDuplicateToolCalls = 0;
};

struct StopHookResult {
  bool preventContinuation = false;
  std::vector<Message> followupMessages;
  std::vector<Message> blockingErrors;
};

class QueryLoop {
 public:
  QueryLoop(tools::ToolOrchestrator& toolOrchestrator,
            permissions::PermissionEngine& permissionEngine,
            api::ModelClient& modelClient,
            api::SideQueryClient& sideQueryClient);

  void SetMaxTurns(int maxTurns);
  void SetWallClockBudget(long long budgetMs);

  void RunFull(QueryLoopContext& ctx);

 private:
  void ApplyStepBudget(QueryLoopContext& ctx,
                       QueryLoopInternalState& state);
  void ApplyStepSnip(QueryLoopContext& ctx,
                     QueryLoopInternalState& state);
  void ApplyStepMicrocompact(QueryLoopContext& ctx,
                             QueryLoopInternalState& state);
  void ApplyStepCollapse(QueryLoopContext& ctx,
                         QueryLoopInternalState& state);
  bool ApplyStepAutocompact(QueryLoopContext& ctx,
                            QueryLoopInternalState& state);
  bool ApplyStepModelCall(QueryLoopContext& ctx,
                          QueryLoopInternalState& state);
  void ApplyStepValidator(QueryLoopContext& ctx,
                          QueryLoopInternalState& state);
  StopHookResult ExecuteStopHooks(QueryLoopContext& ctx,
                                  QueryLoopInternalState& state);
  bool ApplyStepRunTools(QueryLoopContext& ctx,
                         QueryLoopInternalState& state);
  bool ApplyStepTerminate(QueryLoopContext& ctx,
                          QueryLoopInternalState& state);
  bool HandleNoToolContinuation(QueryLoopContext& ctx,
                                QueryLoopInternalState& state);
  bool ContinueWithFollowup(QueryLoopContext& ctx,
                            QueryLoopInternalState& state,
                            const std::vector<Message>& followups,
                            TransitionReason reason,
                            bool resetTurnCount);
  bool ShouldForceContinuation(const QueryLoopContext& ctx,
                               const QueryLoopInternalState& state) const;
  std::vector<Message> BuildMessagesForTurn(
      const QueryLoopContext& ctx,
      const QueryLoopInternalState& state) const;
  void PostToolTurnProcessing(QueryLoopContext& ctx,
                              QueryLoopInternalState& state);
  void AppendTurnArtifacts(QueryLoopContext& ctx,
                           const std::vector<Message>& assistantMessages,
                           const std::vector<Message>& toolResults,
                           const std::vector<Message>& followups) const;

  bool Handle413Recovery(QueryLoopContext& ctx,
                         QueryLoopInternalState& state);
  bool HandleMaxOutputTokens(QueryLoopContext& ctx,
                             QueryLoopInternalState& state);
  bool HandleTokenBudget(QueryLoopContext& ctx,
                         QueryLoopInternalState& state);

  static int EstimateTokens(const std::string& text);
  static int EstimateMessageTokens(const std::vector<Message>& msgs);
  static std::vector<Message> DoCollapseCompact(
      const std::vector<Message>& input, int keepRecent);
  static std::vector<Message> DoReactiveCompact(
      const std::vector<Message>& input);
  static std::vector<Message> DoHistorySnip(
      const std::vector<Message>& input);
  static int CountToolResultBytes(const Message& msg);
  static bool IsPromptTooLong(const Message& msg);
  // P0-02: Duplicate tool call fingerprinting
  std::string MakeToolFingerprint(const ContentBlock& block) const;
  bool ShouldTerminateOnDuplicates(
      QueryLoopContext& ctx,
      QueryLoopInternalState& state) const;
  // P0-02: Wall-clock budget check
  bool IsWallClockExpired(QueryLoopContext& ctx) const;

  tools::ToolOrchestrator& toolOrchestrator_;
  permissions::PermissionEngine& permissionEngine_;
  api::ModelClient& modelClient_;
  api::SideQueryClient& sideQueryClient_;
  int maxTurns_ = 0;
  long long wallClockBudgetMs_ = 0;
  long long loopStartTimeMs_ = 0;
};

}  // namespace core
}  // namespace agent
