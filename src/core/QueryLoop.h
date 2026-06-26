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
  int validatorRetryCount = 0;
  int totalValidatorRetryCount = 0;  // Session-wide, never reset
  int validatorNudgeCount = 0;
  std::string lastValidatorGuidance;
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
  int consecutiveExplorationOnlyTurns = 0;
  // P0-03: Track last assistant message time for time-based microcompact
  long long lastAssistantTimestampMs = 0;
  // P0-2: Track file write operations for mandatory run verification
  bool lastTurnHadFileWrite = false;
  int consecutiveWriteWithoutVerifyCount = 0;
  int verificationNudgeCount = 0;
  // P1-2: Track tool execution errors for error-driven repair loop
  int lastTurnErrorCount = 0;
  int consecutiveErrorTurns = 0;
  std::string lastErrorSummary;
  // P0-3: Track completion nudges to prevent silent termination
  int completionNudgeCount = 0;
  // FIX-C: Track how many tool calls this turn were denied/ask-blocked by the
  // permission gate (e.g. "requires confirmation" in non-interactive mode).
  // Reset at the start of each model call. When the agent has written files
  // but cannot run them (Bash blocked), the completion-nudge ("you MUST run
  // tests, use Bash") would otherwise loop forever; this counter lets
  // ApplyStepTerminate detect that and terminate cleanly instead.
  int recentPermissionDeniedCount = 0;
  // FIX-C: session-wide count of permission-denied tool results, so a runaway
  // nudge loop is bounded even across turns.
  int totalPermissionDeniedCount = 0;
  // FIX-E1 (weak-model support): cached original user goal for the task anchor.
  // Captured once from the first non-meta user message and re-injected at the
  // front of every turn so weak models do not drift off-task (observed: Gemma
  // built a 'task_manager' project instead of the requested 'md2html'). Empty
  // = not yet captured or no user goal found (anchor suppressed).
  std::string cachedTaskAnchor;
  bool taskAnchorCaptured = false;
  // FIX-E4 (weak-model support): text-loop detection. Weak models sometimes
  // emit near-identical prose turn after turn (paraphrased repetition) while
  // taking no productive action — the existing duplicate-TOOL detector misses
  // this because the tool calls differ. We keep a small ring buffer of
  // normalized assistant text and terminate when repetition persists.
  std::vector<std::string> recentAssistantTextFingerprints;
  int textLoopRecoveryAttempts = 0;
  // P0-3: Track result-check nudges for suspicious output after Bash runs
  int resultCheckNudgeCount = 0;
  // P0-4: Give one forced-action nudge before hard-stopping exploration loops
  int explorationActionNudgeCount = 0;
  // GEMMA-ENHANCE: Same-file edit loop breaker
  // Tracks consecutive failed edit attempts on the same file to detect
  // tunnel vision / stuck-loop behavior where the model repeatedly tries
  // to fix one line without success.
  std::string lastEditedFilePath;
  int consecutiveSameFileEditFailures = 0;
  // P0-2: Zero-reasoning tool-call loop breaker. The agent log shows Gemma
  // emitting turns with textEvents=0 toolEvents=1 (tool call, zero reasoning
  // text) repeatedly while the loop keeps ForceContinuing. The textEvents/
  // toolEvents counts live only in ModelClient and never reached QueryLoop,
  // so there was no backstop. lastTurnTextChars captures the reasoning text
  // length of the most recent model call; consecutiveToolOnlyTurns counts
  // how many turns in a row produced tool calls with negligible reasoning.
  // HandleNoToolContinuation hard-terminates ("tool_only_loop") when this
  // exceeds the per-family maxToolOnlyTurns policy.
  int lastTurnTextChars = 0;
  int consecutiveToolOnlyTurns = 0;
  // R2-1: set in ApplyStepModelCall when a turn produced NO tool_use blocks
  // but emitted reasoning text >= policy.maxTextOnlyOutputChars. Such turns
  // are oversized planning monologues (observed 1432 events / 75s on Gemma);
  // nudging them has a 33% failure rate, so HandleNoToolContinuation
  // hard-terminates ("oversized_planning_text") instead of nudging again.
  bool lastTurnWasOversizedPlan = false;
  // R2-2: session-wide ForcedContinuation counter (never reset, mirroring
  // totalValidatorRetryCount). Capped by policy.maxForcedContinuations to
  // prevent runaway nudge cycles (observed 19 forced continuations / 23 min).
  int totalForcedContinuations = 0;
  // STRENGTHEN-T09: validator effectiveness sliding window. Records whether
  // each validator intervention was accepted by the main model on the next
  // turn. When effectiveness drops below 0.3, the tier auto-downgrades to
  // avoid burning turns on ignored guidance.
  struct ValidatorOutcome {
    std::string guidanceHash;  // fingerprint of the guidance given
    bool resolved = false;     // did the main model change behavior?
  };
  std::vector<ValidatorOutcome> validatorOutcomes;  // capped at 20
  // STRENGTHEN-T10: execution contract generated by the validator on the
  // first turn (Strong tier only). Captures goal/constraints/forbidden/
  // acceptance_criteria so post-turn validation can verify against it
  // rather than free-form critique. Empty when not generated.
  std::string executionContract;
  bool executionContractGenerated = false;

  // P0-2: Goal Verifier state (aligned with MiMo Code Goal mechanism).
  // Independent completion verification before the agent terminates.
  // Prevents "false completion" where the agent prematurely declares done.
  struct GoalVerifierState {
    std::string goalCondition;       // extracted from user prompt or contract
    int verificationAttempts = 0;    // how many times we've checked
    int maxAttempts = 5;             // hard cap to prevent infinite loops
    bool goalSpecified = false;      // whether a goal condition was found
    bool lastVerificationPassed = false;  // last check result
  };
  GoalVerifierState goalVerifier;

  // P0-1: Checkpoint incremental mechanism (aligned with MiMo Code checkpoint).
  // At 25%/50%/75% of context window usage, generates a structured summary
  // of progress so far (intent, files touched, errors) and injects it as a
  // meta message. This keeps the model focused and enables Writer SubAgent
  // (Goal ⑤) to consume checkpoint state asynchronously.
  int checkpointPhase = 0;                // 0=none, 1=25%, 2=50%, 3=75%
  std::vector<std::string> checkpointSummaries;  // collected summaries
};

// STRENGTHEN-T09: compute validator effectiveness from the sliding window.
// Returns accepted/total ratio in [0,1]. Returns 1.0 if insufficient samples
// (don't downgrade on thin data). Defined inline in the header so the
// QueryLoop and QueryEngine can both call it.
inline double ComputeValidatorEffectiveness(
    const std::vector<QueryLoopInternalState::ValidatorOutcome>& outcomes) {
  if (outcomes.size() < 10) return 1.0;  // need >=10 samples to judge
  int resolved = 0;
  for (const auto& o : outcomes) if (o.resolved) ++resolved;
  return static_cast<double>(resolved) / static_cast<double>(outcomes.size());
}

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
  // P0-1: Checkpoint — incremental progress summary at context thresholds.
  void ApplyStepCheckpoint(QueryLoopContext& ctx,
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
  // STRENGTHEN-T10: generate an execution contract on the first turn
  // (Strong tier only) so post-turn validation verifies against concrete
  // criteria instead of free-form critique.
  void GenerateExecutionContract(QueryLoopContext& ctx,
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
  std::vector<Message> BuildMessagesForTurn(
      const QueryLoopContext& ctx,
      QueryLoopInternalState& state) const;
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
  static int CountToolResultBytes(const Message& msg);
  static bool IsPromptTooLong(const Message& msg);
  // P0-02: Duplicate tool call fingerprinting
  std::string MakeToolFingerprint(const ContentBlock& block) const;
  bool HandleExcessiveExploration(
      QueryLoopContext& ctx,
      QueryLoopInternalState& state);
  bool ShouldTerminateOnDuplicates(
      QueryLoopContext& ctx,
      QueryLoopInternalState& state) const;
  // FIX-E4: detect near-identical assistant prose repeated across turns and
  // escalate (mild nudge → strong nudge → terminate). Returns true if a
  // recovery nudge was injected (caller should continue the loop) or false
  // when no action taken / hard-termination was applied (caller checks
  // state.completed).
  bool HandleTextLoop(QueryLoopContext& ctx,
                      QueryLoopInternalState& state);
  // P0-2: Goal Verifier — independent completion check before termination.
  // Uses SideQueryClient to verify if the user's goal is truly satisfied.
  // Returns true if the agent should CONTINUE (goal not met), false if OK to stop.
  bool VerifyGoalCompletion(QueryLoopContext& ctx,
                            QueryLoopInternalState& state);
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
