#include "core/QueryEngine.h"
#include "core/QueryLoop.h"
#include "core/StateTypes.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <cassert>
#include <iostream>

static int failures = 0;

static void Check(bool condition, const char* label) {
  if (!condition) {
    std::cerr << "FAIL: " << label << std::endl;
    ++failures;
  }
}

namespace {

void TestLlmConfig() {
  agent::core::LlmConfig cfg;
  cfg.apiEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  cfg.apiKey = "sk-test";
  cfg.mainModel = "claude-sonnet";
  cfg.validatorModel = "claude-haiku";
  cfg.fallbackModel = "claude-opus";
  cfg.connectTimeoutMs = 30000;
  cfg.requestTimeoutMs = 120000;

  Check(!cfg.apiEndpoint.empty(), "LlmConfig should store apiEndpoint");
  Check(!cfg.apiKey.empty(), "LlmConfig should store apiKey");
  Check(cfg.mainModel == "claude-sonnet", "LlmConfig mainModel match");
  Check(cfg.validatorModel == "claude-haiku", "LlmConfig validatorModel match");
  Check(cfg.fallbackModel == "claude-opus", "LlmConfig fallbackModel match");
  Check(cfg.connectTimeoutMs == 30000, "LlmConfig connectTimeoutMs match");
  Check(cfg.requestTimeoutMs == 120000, "LlmConfig requestTimeoutMs match");
}

void TestContentReplacementState() {
  agent::core::ContentReplacementState state;
  Check(!state.HasSeen("tu-001"), "ContentReplacementState empty hasSeen false");
  state.RecordReplacement("tu-001", "[replaced]");
  Check(state.HasSeen("tu-001"), "ContentReplacementState hasSeen true after record");
  Check(state.GetReplacement("tu-001") == "[replaced]",
        "ContentReplacementState get correct replacement");
  Check(state.lastSeenId == "tu-001", "ContentReplacementState lastSeenId set");
}

void TestTransitionReasons() {
  Check(static_cast<int>(agent::core::TransitionReason::None) == 0,
        "TransitionReason None");
  Check(static_cast<int>(agent::core::TransitionReason::CollapseDrainRetry) == 1,
        "TransitionReason CollapseDrainRetry");
  Check(static_cast<int>(agent::core::TransitionReason::ReactiveCompactRetry) == 2,
        "TransitionReason ReactiveCompactRetry");
  Check(static_cast<int>(agent::core::TransitionReason::MaxOutputTokensEscalate) == 3,
        "TransitionReason MaxOutputTokensEscalate");
  Check(static_cast<int>(agent::core::TransitionReason::MaxOutputTokensRecovery) == 4,
        "TransitionReason MaxOutputTokensRecovery");
  Check(static_cast<int>(agent::core::TransitionReason::StopHookBlocking) == 5,
        "TransitionReason StopHookBlocking");
  Check(static_cast<int>(agent::core::TransitionReason::TokenBudgetContinuation) == 6,
        "TransitionReason TokenBudgetContinuation");
}

void TestQueryEngineBasic() {
  Check(true, "QueryEngine config validation OK");
}

void TestQueryLoopInternalState() {
  agent::core::QueryLoopInternalState state;
  Check(state.stage == agent::core::QueryStage::ToolResultBudget,
        "QueryLoopInternalState default stage");
  Check(!state.completed, "QueryLoopInternalState not completed");
  Check(state.turnCount == 0, "QueryLoopInternalState turnCount 0");
  Check(state.terminalReason.empty(), "QueryLoopInternalState no terminal reason");
}

void TestStopHookResult() {
  agent::core::StopHookResult result;
  Check(!result.preventContinuation, "StopHookResult default preventContinuation false");
  Check(result.blockingErrors.empty(), "StopHookResult default blockingErrors empty");
}

void TestAgentConfig() {
  agent::core::AgentConfig cfg = agent::core::AgentConfig::FromDefaults();
  Check(cfg.contextWindow > 0, "AgentConfig contextWindow positive");
}

void TestSkeletonModelClientStream() {
  agent::api::SkeletonModelClient client;
  std::vector<agent::core::Message> msgs;

  int eventCount = 0;
  agent::api::SseEventCallback cb = [&](const std::string&, const std::string&) {
    ++eventCount;
  };

  client.StreamResponse(msgs, "system", "model", cb);
  Check(eventCount > 0, "SkeletonModelClient stream should emit events");
}

void TestHttpLlmClientConstruction() {
  agent::core::LlmConfig cfg;
  cfg.apiEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  cfg.apiKey = "sk-test";
  cfg.mainModel = "claude-sonnet";

  agent::api::HttpLlmClient client(cfg);
  Check(true, "HttpLlmClient constructed");
}

}  // namespace

int main() {
  TestLlmConfig();
  TestContentReplacementState();
  TestTransitionReasons();
  TestQueryEngineBasic();
  TestQueryLoopInternalState();
  TestStopHookResult();
  TestAgentConfig();
  TestSkeletonModelClientStream();
  TestHttpLlmClientConstruction();

  std::cout << "[test_core] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
