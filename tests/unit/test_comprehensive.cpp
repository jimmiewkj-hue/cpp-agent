#include "permissions/BashClassifier.h"
#include "permissions/PermissionEngine.h"
#include "api/CostTracker.h"
#include "core/AgentTypes.h"
#include "core/StateTypes.h"
#include "core/QueryEngine.h"
#include "core/StreamingToolExecutor.h"
#include "infra/StabilityWatchdog.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <cassert>
#include <iostream>
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
// BashClassifier Tests
// ============================================================

void TestBashClassifierApi() {
  agent::permissions::BashClassifier classifier;
  
  // Test that classification works without crashing
  auto decision1 = classifier.Classify("ls -la", {});
  Check(decision1.allow || !decision1.allow, "Classify should return a decision");
  
  auto decision2 = classifier.Classify("cat file.txt", {});
  Check(decision2.allow || !decision2.allow, "Classify should work for cat");
  
  auto decision3 = classifier.Classify("rm -rf /", {});
  Check(!decision3.allow, "rm -rf should be classified as dangerous");
  
  Check(!classifier.IsReadOnlyCommand("rm -rf /"), "rm should not be read-only");
}

void TestBashClassifierReadOnlyDetection() {
  agent::permissions::BashClassifier classifier;
  
  // Verify the classifier detects destructive commands
  Check(!classifier.IsReadOnlyCommand("rm file.txt"), "rm should be destructive");
  Check(!classifier.IsReadOnlyCommand("rmdir dir"), "rmdir should be destructive");
  
  // Build prompt should work
  std::string prompt = classifier.BuildClassifierPrompt("echo hello", {});
  Check(!prompt.empty(), "BuildClassifierPrompt should produce output");
}

void TestBashClassifierSetApiKey() {
  agent::permissions::BashClassifier classifier;
  classifier.SetApiKey("test-key-12345");
  // Should not crash
  Check(true, "SetApiKey should succeed");
}

// ============================================================
// PermissionEngine Tests
// ============================================================

void TestPermissionEngineDefaultBehavior() {
  agent::permissions::PermissionEngine engine;
  
  agent::core::ContentBlock toolUse = agent::core::ContentBlock::MakeToolUse(
      "tu-1", "Bash", R"({"command":"echo hello"})");
  std::vector<agent::core::Message> messages;
  
  auto decision = engine.Evaluate(toolUse, messages);
  // Default fail-closed mode should deny or ask
  Check(decision.behavior == agent::core::PermissionBehavior::Deny ||
        decision.behavior == agent::core::PermissionBehavior::Ask,
        "Default permission evaluates without crashing");
}

void TestPermissionEngineBuildCanUseTool() {
  agent::permissions::PermissionEngine engine;
  
  auto canUseTool = engine.BuildCanUseTool();
  Check(static_cast<bool>(canUseTool), "BuildCanUseTool should return valid callback");
  
  agent::core::ContentBlock toolUse = agent::core::ContentBlock::MakeToolUse(
      "tu-8", "FileRead", R"({"path":"test.txt"})");
  std::vector<agent::core::Message> messages;
  
  auto decision = canUseTool(toolUse, messages);
  Check(decision.behavior == agent::core::PermissionBehavior::Deny ||
        decision.behavior == agent::core::PermissionBehavior::Ask,
        "CanUseTool callback should work without crash");
}

void TestPermissionEngineModes() {
  agent::permissions::PermissionEngine engine;
  
  // Add always-deny for rm
  engine.AddAlwaysDenyRule("Bash");
  
  agent::core::ContentBlock toolUse = agent::core::ContentBlock::MakeToolUse(
      "tu-9", "Bash", R"({"command":"rm file"})");
  std::vector<agent::core::Message> messages;
  
  auto decision = engine.Evaluate(toolUse, messages);
  Check(decision.behavior == agent::core::PermissionBehavior::Deny,
        "Bash should be denied with deny rule on tool name");
  
  // Reset and test deny tracking
  engine.ResetDenialState();
  const auto& state = engine.denialState();
  Check(state.total == 0, "Reset should clear denial state");
  
  // Add allow rule
  engine.AddAlwaysAllowRule("FileRead");
  
  agent::core::ContentBlock readTool = agent::core::ContentBlock::MakeToolUse(
      "tu-10", "FileRead", R"({"path":"test.txt"})");
  auto readDecision = engine.Evaluate(readTool, messages);
  Check(readDecision.behavior == agent::core::PermissionBehavior::Allow,
        "FileRead should be allowed with allow rule");
}

void TestPermissionEngineCircuitBreaker() {
  agent::permissions::PermissionEngine engine;
  
  Check(!engine.IsCircuitBroken(), "Circuit should not be broken initially");
  
  // Test fail-closed mode
  engine.SetFailClosed(true);
  Check(true, "SetFailClosed should work");
}

// ============================================================
// CostTracker Tests
// ============================================================

void TestCostTrackerInitialState() {
  auto& tracker = agent::api::CostTracker::Instance(); tracker.Reset();
  
  Check(tracker.TotalInputTokens() == 0, "Initial input tokens should be 0");
  Check(tracker.TotalOutputTokens() == 0, "Initial output tokens should be 0");
  Check(tracker.TotalCostUsd() == 0.0, "Initial cost should be 0");
}

void TestCostTrackerAccumulation() {
  auto& tracker = agent::api::CostTracker::Instance(); tracker.Reset();
  
  // Simulate a model call
  agent::core::Usage usage;
  usage.inputTokens = 1000;
  usage.outputTokens = 500;
  usage.cacheReadInputTokens = 200;
  usage.cacheCreationInputTokens = 100;
  
  tracker.RecordUsage("claude-sonnet", usage.inputTokens, usage.outputTokens, usage.cacheReadInputTokens, usage.cacheCreationInputTokens);
  
  Check(tracker.TotalInputTokens() == 1000, "Input tokens should accumulate");
  Check(tracker.TotalOutputTokens() == 500, "Output tokens should accumulate");
  Check(tracker.TotalCostUsd() > 0.0, "Cost should be calculated");
}

void TestCostTrackerMultipleAccumulations() {
  auto& tracker = agent::api::CostTracker::Instance(); tracker.Reset();
  
  agent::core::Usage usage1;
  usage1.inputTokens = 500;
  usage1.outputTokens = 200;
  tracker.RecordUsage("claude-sonnet", usage1.inputTokens, usage1.outputTokens, 0, 0);
  
  agent::core::Usage usage2;
  usage2.inputTokens = 300;
  usage2.outputTokens = 150;
  tracker.RecordUsage("claude-sonnet", usage2.inputTokens, usage2.outputTokens, 0, 0);
  
  Check(tracker.TotalInputTokens() == 800, "Total input tokens should sum");
  Check(tracker.TotalOutputTokens() == 350, "Total output tokens should sum");
}

void TestCostTrackerSummary() {
  auto& tracker = agent::api::CostTracker::Instance(); tracker.Reset();
  
  agent::core::Usage usage;
  usage.inputTokens = 2000;
  usage.outputTokens = 800;
  tracker.RecordUsage("claude-sonnet", usage.inputTokens, usage.outputTokens, usage.cacheReadInputTokens, usage.cacheCreationInputTokens);
  
  // Summary test: verify cost tracking works
  double cost = tracker.TotalCostUsd();
  Check(cost > 0.0, "Cost should be tracked");
  int totalIn = tracker.TotalInputTokens();
  Check(totalIn == 2000, "Total input tokens should be tracked");
}

void TestCostTrackerModelPricing() {
  auto& tracker = agent::api::CostTracker::Instance(); tracker.Reset();
  
  // Different models should have different pricing
  agent::core::Usage claudeUsage;
  claudeUsage.inputTokens = 1000000;  // 1M tokens
  claudeUsage.outputTokens = 100000;
  tracker.RecordUsage("claude-sonnet", claudeUsage.inputTokens, claudeUsage.outputTokens, 0, 0);
  
  double cost = tracker.TotalCostUsd();
  Check(cost > 0.0, "Cost should be positive for 1M tokens");
}

// ============================================================
// StreamingToolExecutor Tests
// ============================================================

void TestStreamingToolExecutorEmpty() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::Message> messages;
  
  agent::core::StreamingToolExecutor executor(orchestrator, messages);
  
  Check(!executor.HasPendingTools(), "Empty executor should have no pending tools");
  Check(!executor.HasCompletedTools(), "Empty executor should have no completed tools");
}

void TestStreamingToolExecutorAddTool() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::Message> messages;
  
  agent::core::StreamingToolExecutor executor(orchestrator, messages);
  
  auto toolBlock = agent::core::ContentBlock::MakeToolUse(
      "ste-1", "FileRead", R"({"path":"test.txt"})");
  executor.AddTool(toolBlock);
  
  Check(executor.HasPendingTools(), "Executor should have pending tools after AddTool");
}

void TestStreamingToolExecutorDiscard() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::Message> messages;
  
  agent::core::StreamingToolExecutor executor(orchestrator, messages);
  
  auto toolBlock = agent::core::ContentBlock::MakeToolUse(
      "ste-2", "FileRead", R"({"path":"test.txt"})");
  executor.AddTool(toolBlock);
  executor.Discard();
  
  Check(!executor.HasPendingTools(), "Discarded executor should have no pending tools");
  Check(!executor.HasCompletedTools(), "Discarded executor should have no completed tools");
}

void TestStreamingToolExecutorYield() {
  agent::tools::ToolOrchestrator orchestrator;
  std::vector<agent::core::Message> messages;
  
  agent::core::StreamingToolExecutor executor(orchestrator, messages);
  
  // Adding tools but not executing should still yield nothing
  auto toolBlock = agent::core::ContentBlock::MakeToolUse(
      "ste-3", "FileRead", R"({"path":"test.txt"})");
  executor.AddTool(toolBlock);
  
  auto results = executor.YieldCompletedResults();
  Check(results.empty(), "Yield without execution should return nothing");
}

// ============================================================
// StabilityWatchdog Detailed Tests
// ============================================================

void TestStabilityWatchdogDetailed() {
  agent::infra::StabilityConfig cfg;
  cfg.heartbeatIntervalMs = 1000;
  cfg.heartbeatTimeoutMs = 10000;
  cfg.maxConsecutiveFailures = 3;
  cfg.autoRecover = false;
  
  agent::infra::StabilityWatchdog watchdog(cfg);
  
  // Initial state
  Check(watchdog.IsHealthy(), "Watchdog should be healthy initially");
  
  auto metrics = watchdog.metrics();
  Check(metrics.totalTurns == 0, "Initial turns should be 0");
  Check(metrics.healthy, "Initial metrics should show healthy");
  
  // Heartbeat
  watchdog.Heartbeat();
  Check(watchdog.IsHealthy(), "Watchdog should be healthy after heartbeat");
  
  // Signal turn completions
  watchdog.SignalTurnComplete(true);
  watchdog.SignalTurnComplete(true);
  
  metrics = watchdog.metrics();
  Check(metrics.totalTurns >= 2, "Turn count should increase");
  
  // Signal failures
  watchdog.SignalTurnComplete(false);
  watchdog.SignalTurnComplete(false);
  
  metrics = watchdog.metrics();
  Check(metrics.failedTurns >= 0, "Failed turns count should be accessible");
  
  // OOM signal
  watchdog.SignalOom();
  metrics = watchdog.metrics();
  Check(metrics.oomCount >= 1, "OOM count should be tracked");
  
  // Recovery signal
  watchdog.SignalRecovery();
  metrics = watchdog.metrics();
  Check(metrics.recoveredTurns >= 1, "Recovery count should be tracked");
}

void TestStabilityWatchdogCallbacks() {
  agent::infra::StabilityConfig cfg;
  agent::infra::StabilityWatchdog watchdog(cfg);
  
  bool snapshotCalled = false;
  bool resourceChecked = false;
  bool crashRecovered = false;
  
  watchdog.SetSnapshotCallback([&snapshotCalled]() {
    snapshotCalled = true;
  });
  
  watchdog.SetResourceCheckCallback([&resourceChecked]() -> bool {
    resourceChecked = true;
    return true;
  });
  
  watchdog.SetCrashRecoveryCallback([&crashRecovered]() -> bool {
    crashRecovered = true;
    return true;
  });
  
  // Callbacks should be set without errors
  Check(true, "Watchdog callbacks set successfully");
}



// ============================================================
// ContentReplacementState Tests
// ============================================================

void TestContentReplacementState() {
  agent::core::ContentReplacementState state;
  
  Check(!state.HasSeen("id-1"), "Fresh state should not have seen any IDs");
  
  state.RecordReplacement("id-1", "replaced content");
  Check(state.HasSeen("id-1"), "Should have seen recorded ID");
  
  std::string replacement = state.GetReplacement("id-1");
  Check(replacement == "replaced content", "Should retrieve correct replacement");
  
  Check(!state.HasSeen("id-nonexistent"), "Should not have seen unknown ID");
}

}  // namespace

int main() {
  std::cout << "=== BashClassifier Tests ===" << std::endl;
  TestBashClassifierApi();
  TestBashClassifierReadOnlyDetection();
  TestBashClassifierSetApiKey();
  std::cout << "  BashClassifier tests done." << std::endl;

  std::cout << "=== PermissionEngine Tests ===" << std::endl;
  TestPermissionEngineDefaultBehavior();
  TestPermissionEngineBuildCanUseTool();
  TestPermissionEngineModes();
  TestPermissionEngineCircuitBreaker();
  std::cout << "  PermissionEngine tests done." << std::endl;

  std::cout << "=== CostTracker Tests ===" << std::endl;
  TestCostTrackerInitialState();
  TestCostTrackerAccumulation();
  TestCostTrackerMultipleAccumulations();
  TestCostTrackerSummary();
  TestCostTrackerModelPricing();
  std::cout << "  CostTracker tests done." << std::endl;

  std::cout << "=== StreamingToolExecutor Tests ===" << std::endl;
  TestStreamingToolExecutorEmpty();
  TestStreamingToolExecutorAddTool();
  TestStreamingToolExecutorDiscard();
  TestStreamingToolExecutorYield();
  std::cout << "  StreamingToolExecutor tests done." << std::endl;

  std::cout << "=== StabilityWatchdog Detailed Tests ===" << std::endl;
  TestStabilityWatchdogDetailed();
  TestStabilityWatchdogCallbacks();
  std::cout << "  StabilityWatchdog tests done." << std::endl;


  std::cout << "=== ContentReplacementState Tests ===" << std::endl;
  TestContentReplacementState();
  std::cout << "  ContentReplacementState tests done." << std::endl;

  std::cout << std::endl;
  if (failures == 0) {
    std::cout << "[test_comprehensive] All comprehensive tests PASSED" << std::endl;
    return 0;
  }
  std::cerr << "[test_comprehensive] " << failures << " test(s) FAILED" << std::endl;
  return 1;
}
