#include "hooks/HookExecutor.h"

#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <sstream>
#include <thread>

using json = nlohmann::json;

namespace agent {
namespace hooks {

HookExecutor::HookExecutor() {
  config_ = std::make_shared<HookConfig>();
  abortFlag_ = &internalAbortFlag_;
}

HookExecutor::HookExecutor(std::shared_ptr<HookConfig> config)
    : config_(std::move(config)) {
  abortFlag_ = &internalAbortFlag_;
}

HookExecutor::~HookExecutor() = default;

void HookExecutor::SetConfig(std::shared_ptr<HookConfig> config) {
  config_ = std::move(config);
}

void HookExecutor::SetProcessRunner(infra::ProcessRunner* processRunner) {
  processRunner_ = processRunner;
}

void HookExecutor::SetWorkspaceRoot(const std::string& workspaceRoot) {
  workspaceRoot_ = workspaceRoot;
}

void HookExecutor::SetWorkspaceTrusted(bool trusted) {
  if (config_) config_->SetWorkspaceTrusted(trusted);
}

void HookExecutor::SetGloballyDisabled(bool disabled) {
  if (config_) config_->SetGloballyDisabled(disabled);
}

// ============================================================
// Parse hook JSON output (mirrors local-ace parseHookOutput)
// ============================================================

ParsedHookOutput HookExecutor::ParseHookOutput(
    const std::string& rawStdout) {
  ParsedHookOutput result;
  result.text = rawStdout;

  // Try to find JSON in the output
  auto braceStart = rawStdout.find('{');
  if (braceStart == std::string::npos) {
    return result;  // No JSON found, treat as plain text
  }

  // Find matching closing brace
  int depth = 0;
  size_t braceEnd = braceStart;
  for (; braceEnd < rawStdout.size(); ++braceEnd) {
    if (rawStdout[braceEnd] == '{') ++depth;
    else if (rawStdout[braceEnd] == '}') {
      --depth;
      if (depth == 0) break;
    }
  }

  if (depth != 0) return result;  // Unbalanced braces

  std::string jsonStr = rawStdout.substr(braceStart, braceEnd - braceStart + 1);

  try {
    auto j = json::parse(jsonStr);
    result.isJson = true;

    if (j.contains("decision")) {
      result.decision = j["decision"].get<std::string>();
    }
    if (j.contains("reason")) {
      result.reason = j["reason"].get<std::string>();
    }
    if (j.contains("continue")) {
      result.continueSession = j["continue"].get<bool>();
    }
    if (j.contains("continueSession")) {
      result.continueSession = j["continueSession"].get<bool>();
    }
    if (j.contains("suppressOutput")) {
      result.suppressOutput = j["suppressOutput"].get<bool>();
    }
    if (j.contains("stopReason")) {
      result.reason = j["stopReason"].get<std::string>();
    }
    if (j.contains("hookEventName")) {
      // It's a structured hook response
      if (j.contains("outcome")) {
        result.decision = j["outcome"].get<std::string>();
      }
    }
    if (j.contains("async")) {
      result.isAsync = j["async"].get<bool>();
    }
  } catch (...) {
    // JSON parse failed, treat as plain text
    result.isJson = false;
  }

  return result;
}

// ============================================================
// Execute a single command hook
// ============================================================

HookResult HookExecutor::ExecuteCommandHook(const HookDefinition& hook,
                                              const std::string& hookEventName,
                                              const std::string& jsonInput,
                                              int timeoutMs) {
  HookResult result;
  result.hookEventName = hookEventName;
  result.hookName = hook.command;

  if (!processRunner_) {
    result.outcome = HookOutcome::NonBlockingError;
    result.stderrText = "No ProcessRunner available";
    return result;
  }

  auto startTime = std::chrono::steady_clock::now();

  // Execute the shell command via ProcessRunner
  infra::ProcessRunOptions options;
  options.executable = "cmd.exe";
  options.arguments = {"/c", hook.command};
  options.stdinData = jsonInput;
  if (!workspaceRoot_.empty()) {
    options.workingDirectory = workspaceRoot_;
  }
  options.timeoutMs = static_cast<unsigned long>(timeoutMs);

  infra::ProcessRunResult procResult = processRunner_->Run(options);

  auto endTime = std::chrono::steady_clock::now();
  result.durationMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          endTime - startTime).count());

  result.exitCode = procResult.exitCode;
  result.stdoutText = procResult.stdoutText;
  result.stderrText = procResult.stderrText;

  if (procResult.timedOut) {
    result.outcome = HookOutcome::Cancelled;
    result.reason = "Hook execution timed out";
    return result;
  }

  if (procResult.spawnFailed) {
    result.outcome = HookOutcome::NonBlockingError;
    result.reason = procResult.errorMessage;
    return result;
  }

  // Parse the output
  auto parsed = ParseHookOutput(procResult.stdoutText);

  if (parsed.isJson) {
    result.reason = parsed.reason;
    result.continueSession = parsed.continueSession;
    result.suppressOutput = parsed.suppressOutput;
    if (!parsed.reason.empty()) {
      result.message.role = core::MessageRole::System;
      result.message.uuid = "hook-json-message";
      result.message.isMeta = true;
      result.message.content.push_back(
          core::ContentBlock::MakeText(parsed.reason));
    }

    if (!parsed.decision.empty()) {
      if (parsed.decision == "block" || parsed.decision == "deny") {
        result.outcome = HookOutcome::Blocking;
        result.permissionDecision = false;
        result.permissionReason = parsed.reason;
        result.shouldStop = true;
      } else if (parsed.decision == "allow" || parsed.decision == "success") {
        result.outcome = HookOutcome::Success;
        result.permissionDecision = true;
        result.permissionReason = parsed.reason;
      }
    }

    if (!parsed.continueSession) {
      result.shouldStop = true;
    }
  } else if (procResult.exitCode != 0) {
    result.outcome = HookOutcome::NonBlockingError;
    result.reason = procResult.stderrText.empty()
        ? procResult.stdoutText : procResult.stderrText;
  } else {
    result.outcome = HookOutcome::Success;
    if (!procResult.stdoutText.empty()) {
      result.message.role = core::MessageRole::System;
      result.message.uuid = "hook-stdout-message";
      result.message.isMeta = true;
      result.message.content.push_back(
          core::ContentBlock::MakeText(procResult.stdoutText));
    }
  }

  return result;
}

// ============================================================
// Execute a callback hook
// ============================================================

HookResult HookExecutor::ExecuteCallbackHook(const HookDefinition& hook,
                                               const HookInput& input,
                                               const std::string& toolUseID,
                                               int hookIndex) {
  (void)hook;
  (void)toolUseID;
  (void)hookIndex;
  HookResult result;
  result.hookEventName = input.hook_event_name;
  result.outcome = HookOutcome::Success;
  return result;
}

// ============================================================
// Internal: run hooks for a specific event
// ============================================================

HookBatchResult HookExecutor::RunHooksForEvent(
    HookEventType event,
    const HookInput& input,
    const std::string& toolUseID,
    const std::string& matchQuery,
    int timeoutMs) {
  HookBatchResult batch;

  // Check global disable conditions
  if (!config_) return batch;
  if (config_->IsHooksGloballyDisabled()) return batch;
  if (config_->IsSimpleMode()) return batch;
  if (!config_->IsWorkspaceTrusted()) return batch;

  auto matchedHooks = config_->GetMatchingHooks(event, input, matchQuery);

  auto startTime = std::chrono::steady_clock::now();

  for (size_t i = 0; i < matchedHooks.size(); ++i) {
    const auto& mh = matchedHooks[i];

    if (abortFlag_ && abortFlag_->load()) break;

    HookResult result;
    result.hookEventName = HookEventTypeToString(event);

    switch (mh.hook.type) {
      case HookType::Command: {
        // Build JSON input
        json j;
        j["hook_event_name"] = HookEventTypeToString(event);
        if (!toolUseID.empty()) j["tool_use_id"] = toolUseID;
        result = ExecuteCommandHook(mh.hook, HookEventTypeToString(event),
                                     j.dump(), timeoutMs);
        break;
      }
      case HookType::Callback: {
        result = ExecuteCallbackHook(mh.hook, input, toolUseID,
                                      static_cast<int>(i));
        break;
      }
      case HookType::HTTP:
      case HookType::Prompt:
      case HookType::Agent:
      default: {
        // These types are placeholders for future implementation
        result.outcome = HookOutcome::Success;
        result.hookEventName = HookEventTypeToString(event);
        break;
      }
    }

    switch (result.outcome) {
      case HookOutcome::Success:
        ++batch.numSuccess;
        break;
      case HookOutcome::Blocking:
        ++batch.numBlocking;
        break;
      case HookOutcome::NonBlockingError:
        ++batch.numNonBlockingError;
        break;
      case HookOutcome::Cancelled:
        ++batch.numCancelled;
        break;
    }

    batch.results.push_back(result);

    // If blocking, stop processing further hooks
    if (result.outcome == HookOutcome::Blocking) break;
  }

  // Also run session hooks
  for (const auto& sh : config_->GetSessionHooks()) {
    if (static_cast<int>(sh.first) == static_cast<int>(event) && sh.second) {
      HookResult r = sh.second(input, toolUseID);
      batch.results.push_back(r);
      switch (r.outcome) {
        case HookOutcome::Success:
          ++batch.numSuccess;
          break;
        case HookOutcome::Blocking:
          ++batch.numBlocking;
          break;
        case HookOutcome::NonBlockingError:
          ++batch.numNonBlockingError;
          break;
        case HookOutcome::Cancelled:
          ++batch.numCancelled;
          break;
      }
      if (r.outcome == HookOutcome::Blocking) break;
    }
  }

  auto endTime = std::chrono::steady_clock::now();
  batch.totalDurationMs = static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          endTime - startTime).count());

  return batch;
}

// ============================================================
// Main execute function
// ============================================================

HookBatchResult HookExecutor::Execute(const HookInput& input,
                                        const std::string& toolUseID,
                                        const std::string& matchQuery,
                                        int timeoutMs) {
  return RunHooksForEvent(input.eventType, input, toolUseID, matchQuery,
                           timeoutMs);
}

// ============================================================
// Convenience methods
// ============================================================

HookBatchResult HookExecutor::RunPreToolUseHooks(
    const std::string& toolName,
    const std::string& toolInput,
    const std::string& toolUseID,
    int timeoutMs) {
  auto input = HookInput::MakePreToolUse(toolName, toolInput, toolUseID);
  return RunHooksForEvent(HookEventType::PreToolUse, input, toolUseID,
                           toolName, timeoutMs);
}

HookBatchResult HookExecutor::RunPostToolUseHooks(
    const std::string& toolName,
    const std::string& toolInput,
    const std::string& toolUseID,
    const std::string& toolOutput,
    int exitCode,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::PostToolUse;
  hi.hook_event_name = "PostToolUse";
  hi.postToolUse.tool_name = toolName;
  hi.postToolUse.tool_input = toolInput;
  hi.postToolUse.tool_use_id = toolUseID;
  hi.postToolUse.tool_output = toolOutput;
  hi.postToolUse.exit_code = exitCode;
  return RunHooksForEvent(HookEventType::PostToolUse, hi, toolUseID,
                           toolName, timeoutMs);
}

HookBatchResult HookExecutor::RunStopHooks(const std::string& stopReason,
                                              int timeoutMs) {
  auto input = HookInput::MakeStop(stopReason);
  return RunHooksForEvent(HookEventType::Stop, input, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunStopFailureHooks(
    const std::string& stopReason,
    const std::string& error,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::StopFailure;
  hi.hook_event_name = "StopFailure";
  hi.stopFailure.stop_reason = stopReason;
  hi.stopFailure.error = error;
  return RunHooksForEvent(HookEventType::StopFailure, hi, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunSessionStartHooks(
    const std::string& sessionId,
    const std::string& cwd,
    int timeoutMs) {
  auto input = HookInput::MakeSessionStart(sessionId, cwd);
  return RunHooksForEvent(HookEventType::SessionStart, input, "", "",
                           timeoutMs);
}

HookBatchResult HookExecutor::RunSessionEndHooks(
    const std::string& sessionId,
    const std::string& reason,
    int timeoutMs) {
  auto input = HookInput::MakeSessionEnd(sessionId, reason);
  return RunHooksForEvent(HookEventType::SessionEnd, input, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunNotificationHooks(
    const std::string& message,
    const std::string& notificationType,
    int timeoutMs) {
  auto input = HookInput::MakeNotification(message, notificationType);
  return RunHooksForEvent(HookEventType::Notification, input, "", "",
                           timeoutMs);
}

HookBatchResult HookExecutor::RunUserPromptSubmitHooks(
    const std::string& prompt,
    int timeoutMs) {
  auto input = HookInput::MakeUserPromptSubmit(prompt);
  return RunHooksForEvent(HookEventType::UserPromptSubmit, input, "", "",
                           timeoutMs);
}

HookBatchResult HookExecutor::RunSubagentStartHooks(
    const std::string& subagentType,
    const std::string& prompt,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::SubagentStart;
  hi.hook_event_name = "SubagentStart";
  hi.subagentStart.subagent_type = subagentType;
  hi.subagentStart.prompt = prompt;
  return RunHooksForEvent(HookEventType::SubagentStart, hi, "", "",
                           timeoutMs);
}

HookBatchResult HookExecutor::RunSubagentStopHooks(
    const std::string& subagentType,
    const std::string& stopReason,
    int exitCode,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::SubagentStop;
  hi.hook_event_name = "SubagentStop";
  hi.subagentStop.subagent_type = subagentType;
  hi.subagentStop.stop_reason = stopReason;
  hi.subagentStop.exit_code = exitCode;
  return RunHooksForEvent(HookEventType::SubagentStop, hi, "", "",
                           timeoutMs);
}

HookBatchResult HookExecutor::RunTaskCreatedHooks(
    const std::string& taskId,
    const std::string& subject,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::TaskCreated;
  hi.hook_event_name = "TaskCreated";
  hi.taskCreated.task_id = taskId;
  hi.taskCreated.subject = subject;
  return RunHooksForEvent(HookEventType::TaskCreated, hi, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunTaskCompletedHooks(
    const std::string& taskId,
    const std::string& subject,
    bool success,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::TaskCompleted;
  hi.hook_event_name = "TaskCompleted";
  hi.taskCompleted.task_id = taskId;
  hi.taskCompleted.subject = subject;
  hi.taskCompleted.success = success;
  return RunHooksForEvent(HookEventType::TaskCompleted, hi, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunPreCompactHooks(
    const std::string& trigger,
    int messageCount,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::PreCompact;
  hi.hook_event_name = "PreCompact";
  hi.preCompact.trigger = trigger;
  hi.preCompact.message_count = messageCount;
  return RunHooksForEvent(HookEventType::PreCompact, hi, "", "", timeoutMs);
}

HookBatchResult HookExecutor::RunPostCompactHooks(
    int messageCountBefore,
    int messageCountAfter,
    int tokensSaved,
    int timeoutMs) {
  HookInput hi;
  hi.eventType = HookEventType::PostCompact;
  hi.hook_event_name = "PostCompact";
  hi.postCompact.message_count_before = messageCountBefore;
  hi.postCompact.message_count_after = messageCountAfter;
  hi.postCompact.tokens_saved = tokensSaved;
  return RunHooksForEvent(HookEventType::PostCompact, hi, "", "", timeoutMs);
}

}  // namespace hooks
}  // namespace agent
