#pragma once

#include "hooks/HookTypes.h"
#include "hooks/HookConfig.h"
#include "infra/ProcessRunner.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {
namespace hooks {

// ============================================================
// Hook Executor
// Mirrors local-ace executeHooks() — the main hook execution engine
// ============================================================

class HookExecutor {
 public:
  HookExecutor();
  explicit HookExecutor(std::shared_ptr<HookConfig> config);
  ~HookExecutor();

  void SetConfig(std::shared_ptr<HookConfig> config);
  void SetProcessRunner(infra::ProcessRunner* processRunner);
  void SetWorkspaceRoot(const std::string& workspaceRoot);

  // ============================================================
  // Main execution function (mirrors local-ace executeHooks)
  // ============================================================
  HookBatchResult Execute(const HookInput& input,
                           const std::string& toolUseID = "",
                           const std::string& matchQuery = "",
                           int timeoutMs = 600000);

  // ============================================================
  // Event-specific convenience methods
  // ============================================================

  // Pre-tool-use hooks: run BEFORE a tool executes
  // Returns: blocking results (decision to allow/deny/ask)
  HookBatchResult RunPreToolUseHooks(const std::string& toolName,
                                      const std::string& toolInput,
                                      const std::string& toolUseID,
                                      int timeoutMs = 600000);

  // Post-tool-use hooks: run AFTER a tool executes
  HookBatchResult RunPostToolUseHooks(const std::string& toolName,
                                       const std::string& toolInput,
                                       const std::string& toolUseID,
                                       const std::string& toolOutput,
                                       int exitCode = 0,
                                       int timeoutMs = 600000);

  // Stop hooks: run when agent is about to stop
  HookBatchResult RunStopHooks(const std::string& stopReason,
                                int timeoutMs = 60000);

  // Stop-failure hooks: run when stop hooks themselves fail
  HookBatchResult RunStopFailureHooks(const std::string& stopReason,
                                       const std::string& error,
                                       int timeoutMs = 30000);

  // Session start hooks
  HookBatchResult RunSessionStartHooks(const std::string& sessionId,
                                        const std::string& cwd,
                                        int timeoutMs = 60000);

  // Session end hooks (short timeout)
  HookBatchResult RunSessionEndHooks(const std::string& sessionId,
                                      const std::string& reason,
                                      int timeoutMs = 1500);

  // Notification hooks
  HookBatchResult RunNotificationHooks(const std::string& message,
                                        const std::string& notificationType = "",
                                        int timeoutMs = 30000);

  // User prompt submit hooks
  HookBatchResult RunUserPromptSubmitHooks(const std::string& prompt,
                                            int timeoutMs = 30000);

  // Subagent lifecycle hooks
  HookBatchResult RunSubagentStartHooks(const std::string& subagentType,
                                         const std::string& prompt,
                                         int timeoutMs = 60000);

  HookBatchResult RunSubagentStopHooks(const std::string& subagentType,
                                        const std::string& stopReason,
                                        int exitCode = 0,
                                        int timeoutMs = 60000);

  // Task lifecycle hooks
  HookBatchResult RunTaskCreatedHooks(const std::string& taskId,
                                       const std::string& subject,
                                       int timeoutMs = 30000);

  HookBatchResult RunTaskCompletedHooks(const std::string& taskId,
                                         const std::string& subject,
                                         bool success = true,
                                         int timeoutMs = 30000);

  // Compact hooks
  HookBatchResult RunPreCompactHooks(const std::string& trigger,
                                      int messageCount,
                                      int timeoutMs = 60000);

  HookBatchResult RunPostCompactHooks(int messageCountBefore,
                                       int messageCountAfter,
                                       int tokensSaved,
                                       int timeoutMs = 60000);

  // Security/state
  void SetWorkspaceTrusted(bool trusted);
  void SetGloballyDisabled(bool disabled);
  void SetAbortFlag(std::atomic<bool>* flag) { abortFlag_ = flag; }

  // Parse hook JSON output (mirrors local-ace parseHookOutput)
  static ParsedHookOutput ParseHookOutput(const std::string& rawStdout);

 private:
  // Execute a single hook (command type)
  HookResult ExecuteCommandHook(const HookDefinition& hook,
                                 const std::string& hookEventName,
                                 const std::string& jsonInput,
                                 int timeoutMs);

  // Execute a callback-style hook
  HookResult ExecuteCallbackHook(const HookDefinition& hook,
                                  const HookInput& input,
                                  const std::string& toolUseID,
                                  int hookIndex);

  // Internal execute
  HookBatchResult RunHooksForEvent(HookEventType event,
                                    const HookInput& input,
                                    const std::string& toolUseID,
                                    const std::string& matchQuery,
                                    int timeoutMs);

  std::shared_ptr<HookConfig> config_;
  infra::ProcessRunner* processRunner_ = nullptr;
  std::string workspaceRoot_;
  std::atomic<bool>* abortFlag_ = nullptr;
  std::atomic<bool> internalAbortFlag_{false};
};

}  // namespace hooks
}  // namespace agent