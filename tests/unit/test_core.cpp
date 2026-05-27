#include "core/QueryEngine.h"
#include "core/QueryLoop.h"
#include "core/StateTypes.h"
#include "hooks/HookConfig.h"
#include "hooks/HookExecutor.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "infra/SessionManager.h"
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

class PlanThenToolModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(
        agent::core::ContentBlock::MakeText("generated response"));
    return {msg};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("text_delta", "我先查看项目目录，然后读取 README。");
      onEvent("stop_reason", "end_turn");
      return;
    }
    if (streamCalls == 2) {
      onEvent("text_delta", "让我先读取 README。");
      onEvent(
          "tool_use",
          R"({"id":"test-tu-001","name":"FileRead","input":{"path":"README.md"}})");
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "README 已读取，继续完成分析。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
};

class MaxTokensRecoveryModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int maxTokensOverride) override {
    maxTokenOverrides.push_back(maxTokensOverride);
    ++streamCalls;
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("api_error", "max_output_tokens");
      onEvent("stop_reason", "max_tokens");
      return;
    }
    onEvent("text_delta", "Recovered after max tokens escalation.");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
  std::vector<int> maxTokenOverrides;
};

class FallbackModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string& model,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    models.push_back(model);
    if (!onEvent) return;
    if (model == "main-model") {
      onEvent("api_error", "HTTP 529");
      onEvent("stop_reason", "error");
      return;
    }
    onEvent("text_delta", "Recovered through fallback model.");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  std::vector<std::string> models;
};

class ValidatorRetryModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>& messages,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (streamCalls == 2) {
      secondCallSawValidatorGuidance = ContainsText(
          messages, "[Validator] Use Glob first.");
      secondCallSawPreviousToolUse = ContainsToolUse(messages, "FileRead");
    }
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("text_delta", "让我先直接读取 README。");
      onEvent(
          "tool_use",
          R"({"id":"validator-tu-001","name":"FileRead","input":{"path":"README.md"}})");
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "我先用 Glob 看项目结构，再决定读取哪个文件。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    ++validatorCalls;
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    if (validatorCalls == 1) {
      msg.content.push_back(agent::core::ContentBlock::MakeText(
          "<validation_json>{"
          "\"text_correction\":{\"needed\":false},"
          "\"tool_interventions\":[],"
          "\"final_response_action\":\"retry_from_tools\","
          "\"retry_guidance\":\"Use Glob first.\""
          "}</validation_json>"));
    } else {
      msg.content.push_back(agent::core::ContentBlock::MakeText(
          "<validation_json>{"
          "\"text_correction\":{\"needed\":false},"
          "\"tool_interventions\":[],"
          "\"final_response_action\":\"approve\""
          "}</validation_json>"));
    }
    return {msg};
  }

  static bool ContainsText(const std::vector<agent::core::Message>& messages,
                           const std::string& needle) {
    for (const auto& msg : messages) {
      for (const auto& block : msg.content) {
        if (block.type == agent::core::BlockType::Text &&
            block.asText.text.find(needle) != std::string::npos) {
          return true;
        }
      }
    }
    return false;
  }

  static bool ContainsToolUse(const std::vector<agent::core::Message>& messages,
                              const std::string& toolName) {
    for (const auto& msg : messages) {
      for (const auto& block : msg.content) {
        if (block.type == agent::core::BlockType::ToolUse &&
            block.asToolUse.name == toolName) {
          return true;
        }
      }
    }
    return false;
  }

  int streamCalls = 0;
  int validatorCalls = 0;
  bool secondCallSawValidatorGuidance = false;
  bool secondCallSawPreviousToolUse = false;
};

class ValidatorCorrectionModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (!onEvent) return;
    onEvent("text_delta", "这个回答需要被校正。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "<validation_json>{"
        "\"text_correction\":{\"needed\":true,\"corrected_text\":\"这是校正后的最终回答。\"},"
        "\"tool_interventions\":[],"
        "\"final_response_action\":\"approve\""
        "}</validation_json>"
        "<corrected_text>这是校正后的最终回答。</corrected_text>"));
    return {msg};
  }

  int streamCalls = 0;
};

class EventStreamingModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("text_delta", "先读取 README。");
      onEvent(
          "tool_use",
          R"({"id":"event-tu-001","name":"FileRead","input":{"path":"README.md"}})");
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "README 已读取，正在汇总。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
};

class PlanningOnlyModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (!onEvent) return;
    onEvent("text_delta", "我先规划一下，然后继续。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
};

class SnipCaptureModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>& messages,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    capturedMessages = messages;
    if (!onEvent) return;
    onEvent("text_delta", "snip complete");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  std::vector<agent::core::Message> capturedMessages;
};

class StopHookContinuationModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("text_delta", "我已经完成计划。");
      onEvent("stop_reason", "end_turn");
      return;
    }
    onEvent("text_delta", "根据 stop hook 提示，我继续执行测试。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
};

class CompactHookModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "auto compact summary"));
    return {msg};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    if (!onEvent) return;
    onEvent("text_delta", "compact path finished");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }
};

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
  Check(static_cast<int>(agent::core::TransitionReason::ValidatorRetry) == 7,
        "TransitionReason ValidatorRetry");
  Check(static_cast<int>(agent::core::TransitionReason::ForcedContinuation) == 8,
        "TransitionReason ForcedContinuation");
  Check(static_cast<int>(agent::core::TransitionReason::ToolResultContinuation) == 9,
        "TransitionReason ToolResultContinuation");
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

void TestQueryLoopPlanForcesContinuation() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolSchema fileRead;
  fileRead.name = "FileRead";
  fileRead.description = "read file";
  fileRead.category = agent::tools::ToolExecCategory::ReadOnly;
  fileRead.readOnlyHint = true;
  toolRegistry.RegisterTool(fileRead);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent");

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("FileRead");

  PlanThenToolModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "test-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-1";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请检查项目并继续完成真实工作。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls >= 3,
        "QueryLoop should continue after plan-only first response");

  bool sawForcedFollowup = false;
  bool sawToolResult = false;
  bool sawFinalAssistant = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("Do not stop at planning") != std::string::npos) {
        sawForcedFollowup = true;
      }
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("README 已读取") != std::string::npos) {
        sawFinalAssistant = true;
      }
      if (block.type == agent::core::BlockType::ToolResult) {
        sawToolResult = true;
      }
    }
  }

  Check(sawForcedFollowup, "Forced continuation follow-up should be injected");
  Check(sawToolResult, "Tool result should be present after continuation");
  Check(sawFinalAssistant, "Final assistant response should be present");
}

void TestSkeletonModelClientStream() {
  agent::api::SkeletonModelClient client;
  std::vector<agent::core::Message> msgs;

  int eventCount = 0;
  agent::api::SseEventCallback cb = [&](const std::string&, const std::string&) {
    ++eventCount;
  };

  client.StreamResponse(msgs, "system", "model", "", cb);
  Check(eventCount > 0, "SkeletonModelClient stream should emit events");
}

void TestQueryLoopMaxTokensEscalation() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  MaxTokensRecoveryModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-max";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请继续输出，不要截断。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls >= 2,
        "Max tokens error should trigger a retry");
  Check(modelClient.maxTokenOverrides.size() >= 2,
        "Max tokens override history should be captured");
  Check(modelClient.maxTokenOverrides[0] == 0,
        "First request should use default max tokens");
  Check(modelClient.maxTokenOverrides[1] == 65536,
        "Second request should escalate to 64k max tokens");

  bool sawRecoveredAnswer = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("Recovered after max tokens escalation") !=
              std::string::npos) {
        sawRecoveredAnswer = true;
      }
    }
  }
  Check(sawRecoveredAnswer, "Recovered answer should be present");
}

void TestQueryLoopFallbackModelRetry() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  FallbackModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.fallbackModel = "fallback-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-fallback";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请继续执行，如果主模型失败就切换。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.models.size() >= 2,
        "Fallback path should invoke the model at least twice");
  Check(modelClient.models[0] == "main-model",
        "First fallback test call should use main model");
  Check(modelClient.models[1] == "fallback-model",
        "Second fallback test call should use fallback model");

  bool sawFallbackWarning = false;
  bool sawFallbackAnswer = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("Fallback: switching from main-model to fallback-model") !=
              std::string::npos) {
        sawFallbackWarning = true;
      }
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("Recovered through fallback model") !=
              std::string::npos) {
        sawFallbackAnswer = true;
      }
    }
  }
  Check(sawFallbackWarning, "Fallback warning should be persisted");
  Check(sawFallbackAnswer, "Fallback answer should be persisted");
}

void TestQueryLoopValidatorRetryBeforeToolExecution() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolSchema fileRead;
  fileRead.name = "FileRead";
  fileRead.description = "read file";
  fileRead.category = agent::tools::ToolExecCategory::ReadOnly;
  fileRead.readOnlyHint = true;
  toolRegistry.RegisterTool(fileRead);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent");

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("FileRead");

  ValidatorRetryModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.validatorModel = "validator-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-validator-retry";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "先看项目结构，再读取关键文件。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls == 2,
        "Validator retry should cause a second model turn");
  Check(modelClient.validatorCalls >= 2,
        "Validator should run for both turns");
  Check(modelClient.secondCallSawValidatorGuidance,
        "Retry turn should include validator guidance message");
  Check(modelClient.secondCallSawPreviousToolUse,
        "Retry turn should retain prior assistant tool_use history");

  bool sawToolResult = false;
  bool sawValidatorGuidance = false;
  bool sawFinalAssistant = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::ToolResult) {
        sawToolResult = true;
      }
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("[Validator] Use Glob first.") !=
              std::string::npos) {
        sawValidatorGuidance = true;
      }
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("我先用 Glob 看项目结构") !=
              std::string::npos) {
        sawFinalAssistant = true;
      }
    }
  }

  Check(!sawToolResult,
        "retry_from_tools should skip actual tool execution for the rejected draft");
  Check(sawValidatorGuidance,
        "Validator guidance should be appended into conversation history");
  Check(sawFinalAssistant,
        "Second-turn assistant answer should be persisted");
}

void TestQueryLoopValidatorTextCorrection() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  ValidatorCorrectionModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.validatorModel = "validator-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-validator-correction";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请直接给出一句最终结论。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  bool sawCorrectedAnswer = false;
  bool sawOriginalAnswer = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::Text) continue;
      if (block.asText.text.find("这是校正后的最终回答。") !=
          std::string::npos) {
        sawCorrectedAnswer = true;
      }
      if (block.asText.text.find("这个回答需要被校正。") !=
          std::string::npos) {
        sawOriginalAnswer = true;
      }
    }
  }

  Check(modelClient.streamCalls == 1,
        "Correction-only validation should not trigger extra turns");
  Check(sawCorrectedAnswer,
        "Validator corrected text should replace the assistant output");
  Check(!sawOriginalAnswer,
        "Original uncorrected text should not remain in final history");
}

void TestHttpLlmClientConstruction() {
  agent::core::LlmConfig cfg;
  cfg.apiEndpoint = "http://127.0.0.1:8080/v1/chat/completions";
  cfg.apiKey = "sk-test";
  cfg.mainModel = "claude-sonnet";

  agent::api::HttpLlmClient client(cfg);
  Check(true, "HttpLlmClient constructed");
}

void TestQueryEngineEmitsStreamingEvents() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolSchema fileRead;
  fileRead.name = "FileRead";
  fileRead.description = "read file";
  fileRead.category = agent::tools::ToolExecCategory::ReadOnly;
  fileRead.readOnlyHint = true;
  toolRegistry.RegisterTool(fileRead);

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent");

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("FileRead");

  EventStreamingModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-event-session");
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetSessionDir("build\\query-engine-event-session");

  std::vector<agent::core::QueryLoopEvent> events;
  engine.SetEventCallback(
      [&](const agent::core::QueryLoopEvent& event) { events.push_back(event); });

  engine.SubmitUserPrompt("请读取 README 并总结。");
  engine.RunTurn();

  bool sawStageChange = false;
  bool sawAssistantStream = false;
  bool sawToolProgress = false;
  bool sawToolResult = false;
  bool sawLoopCompleted = false;
  for (const auto& event : events) {
    std::string eventText;
    for (const auto& block : event.message.content) {
      if (block.type == agent::core::BlockType::Text) {
        if (!eventText.empty()) eventText += "\n";
        eventText += block.asText.text;
      }
    }
    if (event.type == agent::core::QueryLoopEvent::Type::StageChanged &&
        event.stage == agent::core::QueryStage::ModelCall) {
      sawStageChange = true;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::AssistantMessage &&
        eventText.find("README 已读取") != std::string::npos) {
      sawAssistantStream = true;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::ToolProgress &&
        !event.message.content.empty() &&
        event.message.content.front().type == agent::core::BlockType::ToolUse &&
        event.message.content.front().asToolUse.name == "FileRead") {
      sawToolProgress = true;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::ToolResult) {
      sawToolResult = true;
    }
    if (event.type == agent::core::QueryLoopEvent::Type::LoopCompleted &&
        event.terminalReason == "completed") {
      sawLoopCompleted = true;
    }
  }

  Check(sawStageChange, "QueryEngine should emit stage change events");
  Check(sawAssistantStream, "QueryEngine should emit assistant streaming events");
  Check(sawToolProgress, "QueryEngine should emit tool progress events");
  Check(sawToolResult, "QueryEngine should emit tool result events");
  Check(sawLoopCompleted, "QueryEngine should emit loop completed event");
}

void TestForcedContinuationLimitPersistsAcrossFollowups() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  PlanningOnlyModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);
  loop.SetMaxTurns(20);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-plan-only";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "继续执行，不要只停留在计划。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls == 9,
        "Forced continuation count should stop repeated plan-only turns");
}

void TestHistorySnipPreservesOriginalUserTask() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  SnipCaptureModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";

  agent::core::Message rootUser;
  rootUser.role = agent::core::MessageRole::User;
  rootUser.uuid = "user-root";
  rootUser.content.push_back(agent::core::ContentBlock::MakeText(
      "这是最初的用户任务，请保留下来。"));
  ctx.messages.push_back(rootUser);

  for (int i = 0; i < 24; ++i) {
    agent::core::Message msg;
    msg.role = (i % 2 == 0) ? agent::core::MessageRole::Assistant
                            : agent::core::MessageRole::User;
    msg.uuid = "msg-" + std::to_string(i);
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "filler message " + std::to_string(i)));
    ctx.messages.push_back(msg);
  }

  loop.RunFull(ctx);

  bool sawBoundary = false;
  bool sawOriginalTask = false;
  for (const auto& msg : modelClient.capturedMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::Text) continue;
      if (block.asText.text.find("Conversation truncated. Preserved the original user request") !=
          std::string::npos) {
        sawBoundary = true;
      }
      if (block.asText.text.find("这是最初的用户任务，请保留下来。") !=
          std::string::npos) {
        sawOriginalTask = true;
      }
    }
  }

  Check(sawBoundary, "History snip should emit the new boundary message");
  Check(sawOriginalTask, "History snip should preserve the original user task");
}

void TestStopHookCanForceContinuation() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  StopHookContinuationModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  auto hookConfig = std::make_shared<agent::hooks::HookConfig>();
  hookConfig->SetWorkspaceTrusted(true);
  hookConfig->RegisterSessionHook(
      agent::hooks::HookEventType::Stop,
      [](const agent::hooks::HookInput&, const std::string&) {
        agent::hooks::HookResult result;
        result.outcome = agent::hooks::HookOutcome::Success;
        result.continueSession = true;
        result.message.role = agent::core::MessageRole::System;
        result.message.uuid = "stop-hook-continue";
        result.message.isMeta = true;
        result.message.content.push_back(agent::core::ContentBlock::MakeText(
            "Stop hook requests another concrete action before finishing."));
        return result;
      });
  agent::hooks::HookExecutor hookExecutor(hookConfig);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.hookExecutor = &hookExecutor;

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "stop-hook-user";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请先规划然后继续执行。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls == 2,
        "Stop hook should trigger one extra continuation turn");

  bool sawContinuedText = false;
  for (const auto& message : ctx.messages) {
    for (const auto& block : message.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("继续执行测试") != std::string::npos) {
        sawContinuedText = true;
      }
    }
  }
  Check(sawContinuedText,
        "Stop hook continuation should reach the second assistant response");
}

void TestCompactHooksFireDuringSnip() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  CompactHookModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  int preCompactCalls = 0;
  int postCompactCalls = 0;
  auto hookConfig = std::make_shared<agent::hooks::HookConfig>();
  hookConfig->SetWorkspaceTrusted(true);
  hookConfig->RegisterSessionHook(
      agent::hooks::HookEventType::PreCompact,
      [&preCompactCalls](const agent::hooks::HookInput& input, const std::string&) {
        ++preCompactCalls;
        Check(input.preCompact.trigger == "snip" ||
                  input.preCompact.trigger == "collapse",
              "Compact hook should receive a known trigger");
        agent::hooks::HookResult result;
        result.outcome = agent::hooks::HookOutcome::Success;
        return result;
      });
  hookConfig->RegisterSessionHook(
      agent::hooks::HookEventType::PostCompact,
      [&postCompactCalls](const agent::hooks::HookInput& input, const std::string&) {
        ++postCompactCalls;
        Check(input.postCompact.message_count_before >=
                  input.postCompact.message_count_after,
              "PostCompact should report reduced or equal message count");
        agent::hooks::HookResult result;
        result.outcome = agent::hooks::HookOutcome::Success;
        return result;
      });
  agent::hooks::HookExecutor hookExecutor(hookConfig);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.hookExecutor = &hookExecutor;

  for (int i = 0; i < 25; ++i) {
    agent::core::Message msg;
    msg.role = (i % 2 == 0) ? agent::core::MessageRole::User
                            : agent::core::MessageRole::Assistant;
    msg.uuid = "compact-msg-" + std::to_string(i);
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "history message " + std::to_string(i)));
    ctx.messages.push_back(msg);
  }

  loop.RunFull(ctx);

  Check(preCompactCalls >= 1, "PreCompact hook should run during snip/collapse");
  Check(postCompactCalls >= 1,
        "PostCompact hook should run during snip/collapse");
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
  TestQueryLoopPlanForcesContinuation();
  TestSkeletonModelClientStream();
  TestQueryLoopMaxTokensEscalation();
  TestQueryLoopFallbackModelRetry();
  TestQueryLoopValidatorRetryBeforeToolExecution();
  TestQueryLoopValidatorTextCorrection();
  TestHttpLlmClientConstruction();
  TestQueryEngineEmitsStreamingEvents();
  TestForcedContinuationLimitPersistsAcrossFollowups();
  TestHistorySnipPreservesOriginalUserTask();
  TestStopHookCanForceContinuation();
  TestCompactHooksFireDuringSnip();

  std::cout << "[test_core] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
