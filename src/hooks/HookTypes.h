#pragma once

#include "core/AgentTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace hooks {

// ============================================================
// Hook Event Types (mirrors local-ace HookEvent)
// ============================================================

enum class HookEventType {
  PreToolUse,
  PostToolUse,
  PostToolUseFailure,
  PermissionDenied,
  Stop,
  StopFailure,
  SubagentStart,
  SubagentStop,
  SessionStart,
  SessionEnd,
  Notification,
  UserPromptSubmit,
  PreCompact,
  PostCompact,
  PermissionRequest,
  TaskCreated,
  TaskCompleted,
  ConfigChange,
  CwdChanged,
  FileChanged,
  Setup,
  InstructionsLoaded,
  TeammateIdle,
  Elicitation,
  ElicitationResult,
};

inline const char* HookEventTypeToString(HookEventType event) {
  switch (event) {
    case HookEventType::PreToolUse: return "PreToolUse";
    case HookEventType::PostToolUse: return "PostToolUse";
    case HookEventType::PostToolUseFailure: return "PostToolUseFailure";
    case HookEventType::PermissionDenied: return "PermissionDenied";
    case HookEventType::Stop: return "Stop";
    case HookEventType::StopFailure: return "StopFailure";
    case HookEventType::SubagentStart: return "SubagentStart";
    case HookEventType::SubagentStop: return "SubagentStop";
    case HookEventType::SessionStart: return "SessionStart";
    case HookEventType::SessionEnd: return "SessionEnd";
    case HookEventType::Notification: return "Notification";
    case HookEventType::UserPromptSubmit: return "UserPromptSubmit";
    case HookEventType::PreCompact: return "PreCompact";
    case HookEventType::PostCompact: return "PostCompact";
    case HookEventType::PermissionRequest: return "PermissionRequest";
    case HookEventType::TaskCreated: return "TaskCreated";
    case HookEventType::TaskCompleted: return "TaskCompleted";
    case HookEventType::ConfigChange: return "ConfigChange";
    case HookEventType::CwdChanged: return "CwdChanged";
    case HookEventType::FileChanged: return "FileChanged";
    case HookEventType::Setup: return "Setup";
    case HookEventType::InstructionsLoaded: return "InstructionsLoaded";
    case HookEventType::TeammateIdle: return "TeammateIdle";
    case HookEventType::Elicitation: return "Elicitation";
    case HookEventType::ElicitationResult: return "ElicitationResult";
  }
  return "Unknown";
}

inline HookEventType ParseHookEventType(const std::string& s) {
  if (s == "PreToolUse") return HookEventType::PreToolUse;
  if (s == "PostToolUse") return HookEventType::PostToolUse;
  if (s == "PostToolUseFailure") return HookEventType::PostToolUseFailure;
  if (s == "PermissionDenied") return HookEventType::PermissionDenied;
  if (s == "Stop") return HookEventType::Stop;
  if (s == "StopFailure") return HookEventType::StopFailure;
  if (s == "SubagentStart") return HookEventType::SubagentStart;
  if (s == "SubagentStop") return HookEventType::SubagentStop;
  if (s == "SessionStart") return HookEventType::SessionStart;
  if (s == "SessionEnd") return HookEventType::SessionEnd;
  if (s == "Notification") return HookEventType::Notification;
  if (s == "UserPromptSubmit") return HookEventType::UserPromptSubmit;
  if (s == "PreCompact") return HookEventType::PreCompact;
  if (s == "PostCompact") return HookEventType::PostCompact;
  if (s == "PermissionRequest") return HookEventType::PermissionRequest;
  if (s == "TaskCreated") return HookEventType::TaskCreated;
  if (s == "TaskCompleted") return HookEventType::TaskCompleted;
  if (s == "ConfigChange") return HookEventType::ConfigChange;
  if (s == "CwdChanged") return HookEventType::CwdChanged;
  if (s == "FileChanged") return HookEventType::FileChanged;
  if (s == "Setup") return HookEventType::Setup;
  if (s == "InstructionsLoaded") return HookEventType::InstructionsLoaded;
  if (s == "TeammateIdle") return HookEventType::TeammateIdle;
  if (s == "Elicitation") return HookEventType::Elicitation;
  if (s == "ElicitationResult") return HookEventType::ElicitationResult;
  return HookEventType::Notification;  // fallback
}

// ============================================================
// Hook Input Types (mirrors local-ace HookInput variants)
// ============================================================

struct PreToolUseHookInput {
  std::string hook_event_name = "PreToolUse";
  std::string tool_name;
  std::string tool_input;
  std::string tool_use_id;
};

struct PostToolUseHookInput {
  std::string hook_event_name = "PostToolUse";
  std::string tool_name;
  std::string tool_input;
  std::string tool_use_id;
  std::string tool_output;
  int exit_code = 0;
};

struct PostToolUseFailureHookInput {
  std::string hook_event_name = "PostToolUseFailure";
  std::string tool_name;
  std::string tool_input;
  std::string tool_use_id;
  std::string error;
};

struct StopHookInput {
  std::string hook_event_name = "Stop";
  std::string stop_reason;
};

struct StopFailureHookInput {
  std::string hook_event_name = "StopFailure";
  std::string stop_reason;
  std::string error;
};

struct NotificationHookInput {
  std::string hook_event_name = "Notification";
  std::string message;
  std::string notification_type;
};

struct UserPromptSubmitHookInput {
  std::string hook_event_name = "UserPromptSubmit";
  std::string prompt;
};

struct SessionStartHookInput {
  std::string hook_event_name = "SessionStart";
  std::string session_id;
  std::string cwd;
};

struct SessionEndHookInput {
  std::string hook_event_name = "SessionEnd";
  std::string session_id;
  std::string exit_reason;
};

struct PreCompactHookInput {
  std::string hook_event_name = "PreCompact";
  std::string trigger;
  int message_count = 0;
};

struct PostCompactHookInput {
  std::string hook_event_name = "PostCompact";
  int message_count_before = 0;
  int message_count_after = 0;
  int tokens_saved = 0;
};

struct SubagentStartHookInput {
  std::string hook_event_name = "SubagentStart";
  std::string subagent_type;
  std::string prompt;
};

struct SubagentStopHookInput {
  std::string hook_event_name = "SubagentStop";
  std::string subagent_type;
  std::string stop_reason;
  int exit_code = 0;
};

struct TaskCreatedHookInput {
  std::string hook_event_name = "TaskCreated";
  std::string task_id;
  std::string subject;
};

struct TaskCompletedHookInput {
  std::string hook_event_name = "TaskCompleted";
  std::string task_id;
  std::string subject;
  bool success = true;
};

// ============================================================
// Hook Command Types (mirrors local-ace HookCommand)
// ============================================================

enum class HookType {
  Command,    // Shell command hook
  Prompt,     // Prompt hook (LLM evaluates)
  Agent,      // Agent hook (spawns sub-agent)
  HTTP,       // HTTP hook
  Callback,   // Internal callback function
};

inline const char* HookTypeToString(HookType t) {
  switch (t) {
    case HookType::Command: return "command";
    case HookType::Prompt: return "prompt";
    case HookType::Agent: return "agent";
    case HookType::HTTP: return "http";
    case HookType::Callback: return "callback";
  }
  return "unknown";
}

struct HookMatcher {
  HookEventType event;
  std::string matchQuery;  // tool name or wildcard match
};

struct HookDefinition {
  HookType type = HookType::Command;
  HookMatcher matcher;
  std::string command;       // for Command type
  std::string prompt;        // for Prompt type
  std::string url;           // for HTTP type
  std::string agentType;     // for Agent type
  int timeoutSec = 600;      // 10 min default
  std::string statusMessage;
  std::vector<std::string> matcherPatterns;
};

struct MatchedHook {
  HookDefinition hook;
  std::string pluginRoot;
  std::string pluginId;
  std::string skillRoot;
};

// ============================================================
// Hook Result Types (mirrors local-ace HookJSONOutput)
// ============================================================

enum class HookOutcome {
  Success,
  Blocking,
  NonBlockingError,
  Cancelled,
};

struct HookResult {
  HookOutcome outcome = HookOutcome::Success;
  std::string hookEventName;
  std::string hookName;
  bool continueSession = true;    // Stop hook: false = stop
  std::string decision;            // Permission hook: allow/deny
  std::string reason;
  core::Message message;           // Optional message to inject
  std::string stdoutText;
  std::string stderrText;
  int exitCode = 0;
  bool suppressOutput = false;
  int durationMs = 0;

  // Permission hook specific
  bool permissionDecision = true;
  std::string permissionReason;
  std::vector<std::string> updatedPermissions;

  // Stop hook specific
  bool shouldStop = false;
};

struct HookBatchResult {
  std::vector<HookResult> results;
  int numSuccess = 0;
  int numBlocking = 0;
  int numNonBlockingError = 0;
  int numCancelled = 0;
  int totalDurationMs = 0;
};

// ============================================================
// Hook Input (generic container, mirrors local-ace HookInput)
// ============================================================

struct HookInput {
  HookEventType eventType = HookEventType::Notification;
  std::string hook_event_name;

  // Variant data (use based on eventType)
  PreToolUseHookInput preToolUse;
  PostToolUseHookInput postToolUse;
  PostToolUseFailureHookInput postToolUseFailure;
  StopHookInput stop;
  StopFailureHookInput stopFailure;
  NotificationHookInput notification;
  UserPromptSubmitHookInput userPromptSubmit;
  SessionStartHookInput sessionStart;
  SessionEndHookInput sessionEnd;
  PreCompactHookInput preCompact;
  PostCompactHookInput postCompact;
  SubagentStartHookInput subagentStart;
  SubagentStopHookInput subagentStop;
  TaskCreatedHookInput taskCreated;
  TaskCompletedHookInput taskCompleted;

  static HookInput MakePreToolUse(const std::string& toolName,
                                   const std::string& toolInput,
                                   const std::string& toolUseId) {
    HookInput hi;
    hi.eventType = HookEventType::PreToolUse;
    hi.hook_event_name = "PreToolUse";
    hi.preToolUse.tool_name = toolName;
    hi.preToolUse.tool_input = toolInput;
    hi.preToolUse.tool_use_id = toolUseId;
    return hi;
  }

  static HookInput MakePostToolUse(const std::string& toolName,
                                    const std::string& toolInput,
                                    const std::string& toolUseId,
                                    const std::string& toolOutput,
                                    int exitCode = 0) {
    HookInput hi;
    hi.eventType = HookEventType::PostToolUse;
    hi.hook_event_name = "PostToolUse";
    hi.postToolUse.tool_name = toolName;
    hi.postToolUse.tool_input = toolInput;
    hi.postToolUse.tool_use_id = toolUseId;
    hi.postToolUse.tool_output = toolOutput;
    hi.postToolUse.exit_code = exitCode;
    return hi;
  }

  static HookInput MakeStop(const std::string& reason) {
    HookInput hi;
    hi.eventType = HookEventType::Stop;
    hi.hook_event_name = "Stop";
    hi.stop.stop_reason = reason;
    return hi;
  }

  static HookInput MakeNotification(const std::string& message,
                                     const std::string& type = "") {
    HookInput hi;
    hi.eventType = HookEventType::Notification;
    hi.hook_event_name = "Notification";
    hi.notification.message = message;
    hi.notification.notification_type = type;
    return hi;
  }

  static HookInput MakeUserPromptSubmit(const std::string& prompt) {
    HookInput hi;
    hi.eventType = HookEventType::UserPromptSubmit;
    hi.hook_event_name = "UserPromptSubmit";
    hi.userPromptSubmit.prompt = prompt;
    return hi;
  }

  static HookInput MakeSessionStart(const std::string& sessionId,
                                     const std::string& cwd) {
    HookInput hi;
    hi.eventType = HookEventType::SessionStart;
    hi.hook_event_name = "SessionStart";
    hi.sessionStart.session_id = sessionId;
    hi.sessionStart.cwd = cwd;
    return hi;
  }

  static HookInput MakeSessionEnd(const std::string& sessionId,
                                   const std::string& reason) {
    HookInput hi;
    hi.eventType = HookEventType::SessionEnd;
    hi.hook_event_name = "SessionEnd";
    hi.sessionEnd.session_id = sessionId;
    hi.sessionEnd.exit_reason = reason;
    return hi;
  }
};

// ============================================================
// Hook Callback Types
// ============================================================

using HookCallback = std::function<HookResult(
    const HookInput& input,
    const std::string& toolUseID,
    bool isAborted,
    int hookIndex)>;

// ============================================================
// Parsed hook output (mirrors local-ace parseHookOutput result)
// ============================================================

struct ParsedHookOutput {
  bool isJson = false;
  std::string decision;
  std::string reason;
  std::string text;
  bool continueSession = true;
  bool suppressOutput = false;
  bool isAsync = false;
};

}  // namespace hooks
}  // namespace agent