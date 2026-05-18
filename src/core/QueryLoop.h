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
};

struct QueryLoopInternalState {
  QueryStage stage = QueryStage::ToolResultBudget;
  std::vector<Message> assistantMessages;
  std::vector<ContentBlock> toolUseBlocks;
  bool completed = false;
  bool validatorRequestedRetry = false;
  std::string terminalReason;
  int turnCount = 0;
  int consecutiveAutoCompactFailures = 0;
  int maxOutputTokensRecoveryCount = 0;
  bool hasAttemptedReactiveCompact = false;
  bool hasAttemptedCollapseDrain = false;
  int maxOutputTokensOverride = 0;
  TransitionReason transition = TransitionReason::None;
};

struct StopHookResult {
  bool preventContinuation = false;
  std::vector<Message> blockingErrors;
};

class QueryLoop {
 public:
  QueryLoop(tools::ToolOrchestrator& toolOrchestrator,
            permissions::PermissionEngine& permissionEngine,
            api::ModelClient& modelClient,
            api::SideQueryClient& sideQueryClient);

  void SetMaxTurns(int maxTurns);

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

  tools::ToolOrchestrator& toolOrchestrator_;
  permissions::PermissionEngine& permissionEngine_;
  api::ModelClient& modelClient_;
  api::SideQueryClient& sideQueryClient_;
  int maxTurns_ = 500;
};

}  // namespace core
}  // namespace agent
