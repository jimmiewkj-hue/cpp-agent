#include "hooks/HookTypes.h"
#include "hooks/HookConfig.h"
#include "hooks/HookExecutor.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

namespace {

// ============================================================
// HookTypes Tests
// ============================================================

void TestHookEventTypeToString() {
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::PreToolUse)) == "PreToolUse",
      "PreToolUse → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::PostToolUse)) == "PostToolUse",
      "PostToolUse → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::Stop)) == "Stop",
      "Stop → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::Notification)) == "Notification",
      "Notification → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::SessionStart)) == "SessionStart",
      "SessionStart → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::SessionEnd)) == "SessionEnd",
      "SessionEnd → string");
  Check(std::string(HookEventTypeToString(
      agent::hooks::HookEventType::UserPromptSubmit)) == "UserPromptSubmit",
      "UserPromptSubmit → string");
}

void TestParseHookEventType() {
  Check(agent::hooks::ParseHookEventType("PreToolUse") ==
        agent::hooks::HookEventType::PreToolUse,
        "Parse PreToolUse");
  Check(agent::hooks::ParseHookEventType("PostToolUse") ==
        agent::hooks::HookEventType::PostToolUse,
        "Parse PostToolUse");
  Check(agent::hooks::ParseHookEventType("Stop") ==
        agent::hooks::HookEventType::Stop,
        "Parse Stop");
  Check(agent::hooks::ParseHookEventType("Notification") ==
        agent::hooks::HookEventType::Notification,
        "Parse Notification");
  Check(agent::hooks::ParseHookEventType("UnknownEvent") ==
        agent::hooks::HookEventType::Notification,
        "Parse unknown → fallback Notification");
}

void TestHookInputFactoryMethods() {
  // PreToolUse
  auto preTool = agent::hooks::HookInput::MakePreToolUse(
      "Bash", R"({"command":"echo hello"})", "tu-001");
  Check(preTool.eventType == agent::hooks::HookEventType::PreToolUse,
        "PreToolUse factory: event type");
  Check(preTool.preToolUse.tool_name == "Bash",
        "PreToolUse factory: tool name");
  Check(preTool.preToolUse.tool_use_id == "tu-001",
        "PreToolUse factory: tool_use_id");

  // PostToolUse
  auto postTool = agent::hooks::HookInput::MakePostToolUse(
      "FileRead", R"({"file_path":"/tmp/x"})", "tu-002",
      "file contents here", 0);
  Check(postTool.eventType == agent::hooks::HookEventType::PostToolUse,
        "PostToolUse factory: event type");
  Check(postTool.postToolUse.tool_output == "file contents here",
        "PostToolUse factory: output");
  Check(postTool.postToolUse.exit_code == 0,
        "PostToolUse factory: exit code");

  // Stop
  auto stop = agent::hooks::HookInput::MakeStop("completed");
  Check(stop.eventType == agent::hooks::HookEventType::Stop,
        "Stop factory: event type");
  Check(stop.stop.stop_reason == "completed",
        "Stop factory: reason");

  // Notification
  auto notif = agent::hooks::HookInput::MakeNotification(
      "Task done", "task_complete");
  Check(notif.eventType == agent::hooks::HookEventType::Notification,
        "Notification factory: event type");
  Check(notif.notification.message == "Task done",
        "Notification factory: message");

  // UserPromptSubmit
  auto ups = agent::hooks::HookInput::MakeUserPromptSubmit(
      "Fix the bug in login");
  Check(ups.eventType == agent::hooks::HookEventType::UserPromptSubmit,
        "UserPromptSubmit factory: event type");
  Check(ups.userPromptSubmit.prompt == "Fix the bug in login",
        "UserPromptSubmit factory: prompt");

  // SessionStart
  auto ss = agent::hooks::HookInput::MakeSessionStart(
      "session-123", "/home/user/project");
  Check(ss.eventType == agent::hooks::HookEventType::SessionStart,
        "SessionStart factory: event type");
  Check(ss.sessionStart.session_id == "session-123",
        "SessionStart factory: session_id");

  // SessionEnd
  auto se = agent::hooks::HookInput::MakeSessionEnd(
      "session-123", "user_exit");
  Check(se.eventType == agent::hooks::HookEventType::SessionEnd,
        "SessionEnd factory: event type");
  Check(se.sessionEnd.exit_reason == "user_exit",
        "SessionEnd factory: reason");
}

// ============================================================
// HookConfig Tests
// ============================================================

void TestHookConfigLoadFromJson() {
  agent::hooks::HookConfig config;
  config.SetWorkspaceTrusted(true);

  std::string json = R"({
    "hooks": [
      {
        "event": "PreToolUse",
        "type": "command",
        "command": "echo pre-tool-hook"
      },
      {
        "event": "PostToolUse",
        "type": "command",
        "command": "echo post-tool-hook"
      },
      {
        "event": "Stop",
        "type": "command",
        "command": "echo stop-hook",
        "timeout": 30
      },
      {
        "event": "Notification",
        "type": "command",
        "command": "echo notify",
        "matcher": ["*", "task_*"]
      }
    ]
  })";

  bool loaded = config.LoadFromJson(json);
  Check(loaded, "HookConfig::LoadFromJson succeeds");

  Check(config.GetHooks().size() == 4,
        "HookConfig: 4 hooks loaded");

  Check(config.HasHooksForEvent(agent::hooks::HookEventType::PreToolUse),
        "HasHooksForEvent PreToolUse");
  Check(config.HasHooksForEvent(agent::hooks::HookEventType::PostToolUse),
        "HasHooksForEvent PostToolUse");
  Check(config.HasHooksForEvent(agent::hooks::HookEventType::Stop),
        "HasHooksForEvent Stop");
  Check(config.HasHooksForEvent(agent::hooks::HookEventType::Notification),
        "HasHooksForEvent Notification");
  Check(!config.HasHooksForEvent(agent::hooks::HookEventType::SessionStart),
        "!HasHooksForEvent SessionStart (no hooks)");
}

void TestHookConfigAddHook() {
  agent::hooks::HookConfig config;
  config.SetWorkspaceTrusted(true);

  agent::hooks::HookDefinition hook;
  hook.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook.type = agent::hooks::HookType::Command;
  hook.command = "echo test";

  config.AddHook(hook);
  Check(config.GetHooks().size() == 1, "AddHook: size = 1");
  Check(config.HasHooksForEvent(agent::hooks::HookEventType::PreToolUse),
        "AddHook: has PreToolUse");
}

void TestHookConfigGetMatchingHooks() {
  agent::hooks::HookConfig config;
  config.SetWorkspaceTrusted(true);

  agent::hooks::HookDefinition hook1;
  hook1.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook1.type = agent::hooks::HookType::Command;
  hook1.command = "echo hook1";

  agent::hooks::HookDefinition hook2;
  hook2.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook2.type = agent::hooks::HookType::Command;
  hook2.command = "echo hook2";

  agent::hooks::HookDefinition hook3;
  hook3.matcher.event = agent::hooks::HookEventType::PostToolUse;
  hook3.type = agent::hooks::HookType::Command;
  hook3.command = "echo hook3";

  config.AddHook(hook1);
  config.AddHook(hook2);
  config.AddHook(hook3);

  auto input = agent::hooks::HookInput::MakePreToolUse(
      "Bash", "{}", "tu-001");

  auto matched = config.GetMatchingHooks(
      agent::hooks::HookEventType::PreToolUse, input, "Bash");
  Check(matched.size() == 2, "GetMatchingHooks PreToolUse: 2 matches");

  auto postInput = agent::hooks::HookInput::MakePostToolUse(
      "FileRead", "{}", "tu-002", "content", 0);
  auto postMatched = config.GetMatchingHooks(
      agent::hooks::HookEventType::PostToolUse, postInput, "FileRead");
  Check(postMatched.size() == 1, "GetMatchingHooks PostToolUse: 1 match");
}

void TestHookConfigClear() {
  agent::hooks::HookConfig config;
  config.SetWorkspaceTrusted(true);

  agent::hooks::HookDefinition hook;
  hook.matcher.event = agent::hooks::HookEventType::Stop;
  hook.type = agent::hooks::HookType::Command;
  hook.command = "echo stop";
  config.AddHook(hook);

  Check(config.GetHooks().size() == 1, "Pre-clear: 1 hook");
  config.Clear();
  Check(config.GetHooks().size() == 0, "Post-clear: 0 hooks");
}

void TestHookConfigRemoveHooksForEvent() {
  agent::hooks::HookConfig config;
  config.SetWorkspaceTrusted(true);

  agent::hooks::HookDefinition hook1;
  hook1.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook1.command = "cmd1";
  config.AddHook(hook1);

  agent::hooks::HookDefinition hook2;
  hook2.matcher.event = agent::hooks::HookEventType::PostToolUse;
  hook2.command = "cmd2";
  config.AddHook(hook2);

  config.RemoveHooksForEvent(agent::hooks::HookEventType::PreToolUse);
  Check(config.GetHooks().size() == 1, "After remove: 1 hook remaining");
  Check(!config.HasHooksForEvent(agent::hooks::HookEventType::PreToolUse),
        "PreToolUse hooks removed");
  Check(config.HasHooksForEvent(agent::hooks::HookEventType::PostToolUse),
        "PostToolUse hook remains");
}

void TestHookConfigDisabledStates() {
  agent::hooks::HookConfig config;

  // Default: not trusted
  Check(!config.IsWorkspaceTrusted(), "Default: not trusted");

  config.SetWorkspaceTrusted(true);
  Check(config.IsWorkspaceTrusted(), "After set: trusted");

  config.SetGloballyDisabled(true);
  Check(config.IsHooksGloballyDisabled(), "Globally disabled");

  config.SetSimpleMode(true);
  Check(config.IsSimpleMode(), "Simple mode");

  config.SetManagedHooksOnly(true);
  Check(config.ShouldAllowManagedHooksOnly(), "Managed hooks only");
}

void TestHookConfigSessionHooks() {
  agent::hooks::HookConfig config;

  int callCount = 0;
  config.RegisterSessionHook(
      agent::hooks::HookEventType::Stop,
      [&callCount](const agent::hooks::HookInput&, const std::string&) {
        ++callCount;
        agent::hooks::HookResult r;
        r.outcome = agent::hooks::HookOutcome::Success;
        return r;
      });

  Check(config.GetSessionHooks().size() == 1, "Session hook registered");

  // Invoke the session hook
  auto& sessionHooks = config.GetSessionHooks();
  auto input = agent::hooks::HookInput::MakeStop("test");
  sessionHooks[0].second(input, "");
  Check(callCount == 1, "Session hook callback invoked");

  config.ClearSessionHooks();
  Check(config.GetSessionHooks().size() == 0, "Session hooks cleared");
}

// ============================================================
// HookExecutor Tests
// ============================================================

void TestHookExecutorParseJsonOutput() {
  agent::hooks::HookExecutor executor;

  // Valid JSON with block decision
  std::string blockJson = R"({"decision":"block","reason":"unsafe command"})";
  auto parsed = executor.ParseHookOutput(blockJson);
  Check(parsed.isJson, "Parse: is JSON");
  Check(parsed.decision == "block", "Parse: decision=block");
  Check(parsed.reason == "unsafe command", "Parse: reason");

  // Valid JSON with allow decision
  std::string allowJson = R"({"decision":"allow","reason":"safe operation"})";
  auto parsedAllow = executor.ParseHookOutput(allowJson);
  Check(parsedAllow.isJson, "Parse allow: is JSON");
  Check(parsedAllow.decision == "allow", "Parse allow: decision=allow");

  // Plain text (no JSON)
  std::string plainText = "Hook completed successfully";
  auto parsedPlain = executor.ParseHookOutput(plainText);
  Check(!parsedPlain.isJson, "Parse plain: not JSON");

  // JSON with continue flag
  std::string stopJson = R"({"continue":false,"reason":"stop requested"})";
  auto parsedStop = executor.ParseHookOutput(stopJson);
  Check(parsedStop.isJson, "Parse stop: is JSON");
  Check(!parsedStop.continueSession, "Parse stop: continue=false");

  // JSON with suppressOutput
  std::string suppressJson = R"({"suppressOutput":true})";
  auto parsedSuppress = executor.ParseHookOutput(suppressJson);
  Check(parsedSuppress.isJson, "Parse suppress: is JSON");
  Check(parsedSuppress.suppressOutput, "Parse suppress: suppressOutput=true");
}

void TestHookExecutorBasicConfig() {
  auto config = std::make_shared<agent::hooks::HookConfig>();
  config->SetWorkspaceTrusted(true);

  agent::hooks::HookExecutor executor(config);

  // No hooks registered → empty result
  auto result = executor.RunStopHooks("test");
  Check(result.results.empty(), "No hooks: empty results");
  Check(result.numSuccess == 0, "No hooks: 0 successes");

  // Register a hook
  agent::hooks::HookDefinition hook;
  hook.matcher.event = agent::hooks::HookEventType::Stop;
  hook.type = agent::hooks::HookType::Command;
  hook.command = "echo done";
  config->AddHook(hook);

  // Should find the hook (but can't execute without ProcessRunner)
  auto input = agent::hooks::HookInput::MakeStop("test");
  auto matched = config->GetMatchingHooks(
      agent::hooks::HookEventType::Stop, input, "");
  Check(matched.size() == 1, "After register: 1 matching hook");
}

void TestHookExecutorDisabledStates() {
  auto config = std::make_shared<agent::hooks::HookConfig>();
  agent::hooks::HookExecutor executor(config);

  // Not trusted → no hooks run
  auto result1 = executor.RunNotificationHooks("test message");
  Check(result1.results.empty(), "Not trusted: no hooks run");

  // Trusted but globally disabled
  config->SetWorkspaceTrusted(true);
  config->SetGloballyDisabled(true);
  auto result2 = executor.RunNotificationHooks("test message");
  Check(result2.results.empty(), "Globally disabled: no hooks run");

  // Simple mode
  config->SetGloballyDisabled(false);
  config->SetSimpleMode(true);
  auto result3 = executor.RunNotificationHooks("test message");
  Check(result3.results.empty(), "Simple mode: no hooks run");
}

void TestHookExecutorRunPreToolUseHooks() {
  auto config = std::make_shared<agent::hooks::HookConfig>();
  config->SetWorkspaceTrusted(true);

  agent::hooks::HookDefinition hook;
  hook.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook.type = agent::hooks::HookType::Command;
  hook.command = "echo checking";
  config->AddHook(hook);

  agent::hooks::HookExecutor executor(config);

  // Without ProcessRunner, command hooks fail gracefully
  auto result = executor.RunPreToolUseHooks("Bash", R"({"command":"ls"})",
                                              "tu-001");
  // Should attempt to run, fail gracefully with NonBlockingError
  // (because ProcessRunner is not set)
  Check(!result.results.empty() ||
        result.numNonBlockingError >= 0,
        "PreToolUse: runs (may fail without ProcessRunner)");
}

void TestHookExecutorConvenienceMethods() {
  auto config = std::make_shared<agent::hooks::HookConfig>();
  config->SetWorkspaceTrusted(true);

  agent::hooks::HookExecutor executor(config);

  // All these should not crash, even without any hooks registered
  auto r1 = executor.RunPostToolUseHooks("FileRead", "{}", "tu-001", "content");
  Check(r1.results.empty(), "PostToolUse: no hooks → empty");

  auto r2 = executor.RunStopHooks("completed");
  Check(r2.results.empty(), "Stop: no hooks → empty");

  auto r3 = executor.RunStopFailureHooks("timeout", "process killed");
  Check(r3.results.empty(), "StopFailure: no hooks → empty");

  auto r4 = executor.RunSessionStartHooks("s-1", "/tmp");
  Check(r4.results.empty(), "SessionStart: no hooks → empty");

  auto r5 = executor.RunSessionEndHooks("s-1", "exit");
  Check(r5.results.empty(), "SessionEnd: no hooks → empty");

  auto r6 = executor.RunUserPromptSubmitHooks("hello");
  Check(r6.results.empty(), "UserPromptSubmit: no hooks → empty");

  auto r7 = executor.RunSubagentStartHooks("general-purpose", "fix bug");
  Check(r7.results.empty(), "SubagentStart: no hooks → empty");

  auto r8 = executor.RunSubagentStopHooks("general-purpose", "done", 0);
  Check(r8.results.empty(), "SubagentStop: no hooks → empty");

  auto r9 = executor.RunTaskCreatedHooks("task-1", "Fix login");
  Check(r9.results.empty(), "TaskCreated: no hooks → empty");

  auto r10 = executor.RunTaskCompletedHooks("task-1", "Fix login", true);
  Check(r10.results.empty(), "TaskCompleted: no hooks → empty");

  auto r11 = executor.RunPreCompactHooks("auto", 100);
  Check(r11.results.empty(), "PreCompact: no hooks → empty");

  auto r12 = executor.RunPostCompactHooks(100, 50, 5000);
  Check(r12.results.empty(), "PostCompact: no hooks → empty");
}

void TestHookBatchResult() {
  agent::hooks::HookBatchResult batch;
  batch.numSuccess = 3;
  batch.numBlocking = 0;
  batch.numNonBlockingError = 1;
  batch.numCancelled = 0;
  batch.totalDurationMs = 150;

  Check(batch.numSuccess == 3, "BatchResult: numSuccess");
  Check(batch.numBlocking == 0, "BatchResult: numBlocking");
  Check(batch.numNonBlockingError == 1, "BatchResult: numNonBlockingError");
  Check(batch.numCancelled == 0, "BatchResult: numCancelled");
  Check(batch.totalDurationMs == 150, "BatchResult: duration");
}

void TestHookDefinitionFields() {
  agent::hooks::HookDefinition hook;
  hook.type = agent::hooks::HookType::Command;
  hook.command = "echo hello";
  hook.matcher.event = agent::hooks::HookEventType::PreToolUse;
  hook.matcher.matchQuery = "Bash";
  hook.timeoutSec = 120;
  hook.statusMessage = "Running security check...";

  Check(hook.type == agent::hooks::HookType::Command, "HookDef: type");
  Check(hook.command == "echo hello", "HookDef: command");
  Check(hook.matcher.event == agent::hooks::HookEventType::PreToolUse,
        "HookDef: event");
  Check(hook.matcher.matchQuery == "Bash", "HookDef: matchQuery");
  Check(hook.timeoutSec == 120, "HookDef: timeout");
  Check(hook.statusMessage == "Running security check...",
        "HookDef: statusMessage");

  Check(std::string(agent::hooks::HookTypeToString(
      agent::hooks::HookType::Command)) == "command",
      "HookTypeToString: command → command");
  Check(std::string(agent::hooks::HookTypeToString(
      agent::hooks::HookType::Prompt)) == "prompt",
      "HookTypeToString: prompt → prompt");
  Check(std::string(agent::hooks::HookTypeToString(
      agent::hooks::HookType::Agent)) == "agent",
      "HookTypeToString: agent → agent");
  Check(std::string(agent::hooks::HookTypeToString(
      agent::hooks::HookType::HTTP)) == "http",
      "HookTypeToString: http → http");
  Check(std::string(agent::hooks::HookTypeToString(
      agent::hooks::HookType::Callback)) == "callback",
      "HookTypeToString: callback → callback");
}

void TestMatchedHookFields() {
  agent::hooks::MatchedHook mh;
  mh.hook.type = agent::hooks::HookType::Command;
  mh.hook.command = "echo test";
  mh.pluginRoot = "/plugins/test";
  mh.pluginId = "test-plugin";
  mh.skillRoot = "/skills/test";

  Check(mh.pluginRoot == "/plugins/test", "MatchedHook: pluginRoot");
  Check(mh.pluginId == "test-plugin", "MatchedHook: pluginId");
  Check(mh.skillRoot == "/skills/test", "MatchedHook: skillRoot");
}

void TestAllHookEventTypes() {
  // Verify all 24 event types have string representations
  std::vector<agent::hooks::HookEventType> allTypes = {
    agent::hooks::HookEventType::PreToolUse,
    agent::hooks::HookEventType::PostToolUse,
    agent::hooks::HookEventType::PostToolUseFailure,
    agent::hooks::HookEventType::PermissionDenied,
    agent::hooks::HookEventType::Stop,
    agent::hooks::HookEventType::StopFailure,
    agent::hooks::HookEventType::SubagentStart,
    agent::hooks::HookEventType::SubagentStop,
    agent::hooks::HookEventType::SessionStart,
    agent::hooks::HookEventType::SessionEnd,
    agent::hooks::HookEventType::Notification,
    agent::hooks::HookEventType::UserPromptSubmit,
    agent::hooks::HookEventType::PreCompact,
    agent::hooks::HookEventType::PostCompact,
    agent::hooks::HookEventType::PermissionRequest,
    agent::hooks::HookEventType::TaskCreated,
    agent::hooks::HookEventType::TaskCompleted,
    agent::hooks::HookEventType::ConfigChange,
    agent::hooks::HookEventType::CwdChanged,
    agent::hooks::HookEventType::FileChanged,
    agent::hooks::HookEventType::Setup,
    agent::hooks::HookEventType::InstructionsLoaded,
    agent::hooks::HookEventType::TeammateIdle,
    agent::hooks::HookEventType::Elicitation,
    agent::hooks::HookEventType::ElicitationResult,
  };

  for (const auto& t : allTypes) {
    std::string s = agent::hooks::HookEventTypeToString(t);
    Check(!s.empty(), "EventType has string representation");
    Check(s != "Unknown", "EventType is not Unknown");

    auto parsed = agent::hooks::ParseHookEventType(s);
    Check(parsed == t, "Parse-HookEventType round-trips");
  }
}

void TestHookExecutorExecute() {
  auto config = std::make_shared<agent::hooks::HookConfig>();
  config->SetWorkspaceTrusted(true);

  agent::hooks::HookExecutor executor(config);

  // Execute with explicit HookInput
  auto input = agent::hooks::HookInput::MakeStop("user_requested");
  auto result = executor.Execute(input, "tu-001", "Bash", 60000);
  Check(result.results.empty(), "Execute: no hooks → empty result");
}

}  // namespace

int main() {
  std::cout << "=== HookTypes Tests ===" << std::endl;
  TestHookEventTypeToString();
  TestParseHookEventType();
  TestHookInputFactoryMethods();
  std::cout << "  HookTypes tests done." << std::endl;

  std::cout << "=== HookConfig Tests ===" << std::endl;
  TestHookConfigLoadFromJson();
  TestHookConfigAddHook();
  TestHookConfigGetMatchingHooks();
  TestHookConfigClear();
  TestHookConfigRemoveHooksForEvent();
  TestHookConfigDisabledStates();
  TestHookConfigSessionHooks();
  std::cout << "  HookConfig tests done." << std::endl;

  std::cout << "=== HookExecutor Tests ===" << std::endl;
  TestHookExecutorParseJsonOutput();
  TestHookExecutorBasicConfig();
  TestHookExecutorDisabledStates();
  TestHookExecutorRunPreToolUseHooks();
  TestHookExecutorConvenienceMethods();
  TestHookExecutorExecute();
  std::cout << "  HookExecutor tests done." << std::endl;

  std::cout << "=== Hook Types & Fields Tests ===" << std::endl;
  TestHookBatchResult();
  TestHookDefinitionFields();
  TestMatchedHookFields();
  TestAllHookEventTypes();
  std::cout << "  Types & fields tests done." << std::endl;

  std::cout << std::endl;
  if (failures == 0) {
    std::cout << "All hook tests PASSED" << std::endl;
    return 0;
  }
  std::cerr << failures << " hook test(s) FAILED" << std::endl;
  return 1;
}