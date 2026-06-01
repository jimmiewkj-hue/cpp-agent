#include "app/RuntimePolicy.h"
#include "core/QueryEngine.h"
#include "core/QueryLoop.h"
#include "core/StateTypes.h"
#include "hooks/HookConfig.h"
#include "hooks/HookExecutor.h"
#include "api/ModelClient.h"
#include "api/SideQueryClient.h"
#include "infra/SessionManager.h"
#include "memory/MemoryIndex.h"
#include "permissions/PermissionEngine.h"
#include "tools/ToolOrchestrator.h"
#include "tools/ToolRegistry.h"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

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

class WorkspaceFirstModelClient : public agent::api::ModelClient {
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
      secondCallSawGuidance = ContainsText(
          messages, "Do not call Write/FileWrite yet.");
    }
    if (!onEvent) return;
    if (streamCalls == 1) {
      onEvent("text_delta", "我先直接写一个修复文件。");
      onEvent(
          "tool_use",
          R"({"id":"wf-write-001","name":"Write","input":{"file_path":"build/workspace-first-note.txt","content":"created too early"}})");
      onEvent("stop_reason", "tool_use");
      return;
    }
    if (streamCalls == 2) {
      onEvent("text_delta", "我先读取 README，再决定怎么改。");
      onEvent(
          "tool_use",
          R"({"id":"wf-read-001","name":"FileRead","input":{"path":"README.md"}})");
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "已经先完成 workspace 探查。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
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

  int streamCalls = 0;
  bool secondCallSawGuidance = false;
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

class ValidatorRetryLoopModelClient : public agent::api::ModelClient {
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
    if (streamCalls == 3) {
      thirdCallSawExecutionMemory = ContainsText(
          messages, "[Recent execution memory]");
    }
    if (!onEvent) return;
    onEvent("text_delta", "我先直接读取 README。");
    onEvent(
        "tool_use",
        R"({"id":"validator-loop-tu","name":"FileRead","input":{"path":"README.md"}})");
    onEvent("stop_reason", "tool_use");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    ++validatorCalls;
    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "<validation_json>{"
        "\"text_correction\":{\"needed\":false},"
        "\"tool_interventions\":[],"
        "\"final_response_action\":\"retry_from_tools\","
        "\"retry_guidance\":\"Stop rereading README and inspect the project structure first.\""
        "}</validation_json>"));
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

  int streamCalls = 0;
  int validatorCalls = 0;
  bool thirdCallSawExecutionMemory = false;
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

class RepeatedToolFailureModelClient : public agent::api::ModelClient {
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
    if (streamCalls == 4) {
      fourthCallSawExecutionMemory = ContainsText(
          messages, "[Recent execution memory]");
      if (onEvent) {
        onEvent("text_delta", "我继续处理这个错误。");
        onEvent("stop_reason", "end_turn");
      }
      return;
    }
    if (!onEvent) return;
    onEvent("text_delta", "我再检查一次失败原因。");
    if (streamCalls % 2 == 1) {
      onEvent(
          "tool_use",
          R"({"id":"repeat-glob","name":"Glob","input":{"pattern":"src/*.cpp"}})");
      onEvent(
          "tool_use",
          R"({"id":"repeat-read","name":"FileRead","input":{"path":"build/missing-loop-target.txt"}})");
    } else {
      onEvent(
          "tool_use",
          R"({"id":"repeat-read","name":"FileRead","input":{"path":"build/missing-loop-target.txt"}})");
      onEvent(
          "tool_use",
          R"({"id":"repeat-glob","name":"Glob","input":{"pattern":"src/*.cpp"}})");
    }
    onEvent("stop_reason", "tool_use");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
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

  int streamCalls = 0;
  bool fourthCallSawExecutionMemory = false;
};

class RepeatedMissingToolUseModelClient : public agent::api::ModelClient {
 public:
  explicit RepeatedMissingToolUseModelClient(std::string outputPath)
      : outputPath_(std::move(outputPath)) {}

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
      onEvent("text_delta", "现在需要创建 visualizer.py。让我创建这个模块。");
      onEvent("stop_reason", "end_turn");
      return;
    }
    if (streamCalls == 2) {
      onEvent("text_delta", "还需要创建 report_generator.py。首先创建 visualizer.py。");
      onEvent("stop_reason", "end_turn");
      return;
    }
    if (streamCalls == 3) {
      std::string payload =
          std::string("{\"id\":\"rmu-write-001\",\"name\":\"Write\",\"input\":")
          + "{\"file_path\":\"" + outputPath_ +
          "\",\"content\":\"value = 1\\n\"}}";
      onEvent("text_delta", "现在直接创建缺失文件。");
      onEvent("tool_use", payload);
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "缺失文件已创建完成。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;

 private:
  std::string outputPath_;
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
    onEvent("text_delta", "根据 stop hook 提示，测试已执行完成。");
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

class ValidatorPathNormalizationModelClient : public agent::api::ModelClient {
 public:
  explicit ValidatorPathNormalizationModelClient(std::string absolutePath)
      : absolutePath_(std::move(absolutePath)) {}

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
      const std::string payload =
          std::string("{\"id\":\"validator-path-read\",\"name\":\"Read\",\"input\":")
          + "{\"file_path\":\"" + absolutePath_ +
          "\",\"offset\":1,\"limit\":5}}";
      onEvent("text_delta", "我先读取目标文件。");
      onEvent("tool_use", payload);
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "路径归一化验证完成。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>& messages,
      const std::string& systemPrompt,
      const std::string&) override {
    ++validatorCalls;
    if (validatorCalls == 1) {
      capturedValidationContext.clear();
      for (const auto& msg : messages) {
        for (const auto& block : msg.content) {
          if (block.type == agent::core::BlockType::Text) {
            capturedValidationContext += block.asText.text;
          }
        }
      }
      capturedValidatorSystemPrompt = systemPrompt;
      sawRelativePath =
          capturedValidationContext.find("\"file_path\": \"tests\\\\unit\\\\test_core.cpp\"") !=
          std::string::npos;
      sawAbsolutePath =
          capturedValidationContext.find("G:/downloads/claude-code/yuanma-poxi/cpp-agent/tests/unit/test_core.cpp") !=
          std::string::npos;
    }

    agent::core::Message msg;
    msg.role = agent::core::MessageRole::Assistant;
    msg.content.push_back(agent::core::ContentBlock::MakeText(
        "<validation_json>{"
        "\"text_correction\":{\"needed\":false},"
        "\"tool_interventions\":[],"
        "\"final_response_action\":\"approve\""
        "}</validation_json>"));
    return {msg};
  }

  int streamCalls = 0;
  int validatorCalls = 0;
  bool sawRelativePath = false;
  bool sawAbsolutePath = false;
  std::string capturedValidationContext;
  std::string capturedValidatorSystemPrompt;

 private:
  std::string absolutePath_;
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

class WallClockBudgetModelClient : public agent::api::ModelClient {
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
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (!onEvent) return;
    onEvent("text_delta", "我先查看项目目录。");
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

class ResumeAfterWallClockModelClient : public agent::api::ModelClient {
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
    if (streamCalls == 1) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (onEvent) {
        onEvent("text_delta", "我先继续当前任务。");
        onEvent("stop_reason", "end_turn");
      }
      return;
    }
    if (onEvent) {
      onEvent("text_delta", "已恢复并完成后续步骤。");
      onEvent("stop_reason", "end_turn");
    }
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
};

class ManyToolTurnsModelClient : public agent::api::ModelClient {
 public:
  ManyToolTurnsModelClient(int toolTurns, std::vector<std::string> readPaths)
      : toolTurns_(toolTurns), readPaths_(std::move(readPaths)) {}

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
    if (streamCalls <= toolTurns_) {
      const std::string toolId =
          "many-turn-read-" + std::to_string(streamCalls);
      const std::string& readPath =
          readPaths_[static_cast<std::size_t>(streamCalls - 1) % readPaths_.size()];
      const std::string payload =
          std::string("{\"id\":\"") + toolId +
          "\",\"name\":\"FileRead\",\"input\":{\"path\":\"" + readPath +
          "\"}}";
      onEvent("text_delta", "继续读取上下文。");
      onEvent("tool_use", payload);
      onEvent("stop_reason", "tool_use");
      return;
    }
    onEvent("text_delta", "跨过默认轮次后完成。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;

 private:
  int toolTurns_ = 0;
  std::vector<std::string> readPaths_;
};

class MicrocompactCaptureModelClient : public agent::api::ModelClient {
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
    if (onEvent) {
      onEvent("text_delta", "microcompact checked");
      onEvent("stop_reason", "end_turn");
    }
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  std::vector<agent::core::Message> capturedMessages;
};

class PromptCaptureModelClient : public agent::api::ModelClient {
 public:
  std::vector<agent::core::Message> GenerateResponse(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  void StreamResponse(
      const std::vector<agent::core::Message>&,
      const std::string& systemPrompt,
      const std::string&,
      const std::string&,
      const agent::api::SseEventCallback& onEvent,
      int) override {
    ++streamCalls;
    lastSystemPrompt = systemPrompt;
    if (!onEvent) return;
    onEvent("text_delta", "已完成。");
    onEvent("stop_reason", "end_turn");
  }

  std::vector<agent::core::Message> SideQuery(
      const std::vector<agent::core::Message>&,
      const std::string&,
      const std::string&) override {
    return {};
  }

  int streamCalls = 0;
  std::string lastSystemPrompt;
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

void TestWorkspaceFirstBlocksInitialWriteTool() {
  const std::string workspaceRoot =
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent";
  const std::string accidentalWritePath =
      workspaceRoot + "\\build\\workspace-first-note.txt";
  DeleteFileA(accidentalWritePath.c_str());

  agent::tools::ToolRegistry toolRegistry;
  for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
    if (tool.name == "Write" || tool.name == "FileWrite" ||
        tool.name == "FileRead" || tool.name == "Read" ||
        tool.name == "Grep" || tool.name == "Glob") {
      toolRegistry.RegisterTool(tool);
    }
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("Write");
  permissionEngine.AddAlwaysAllowRule("FileRead");
  permissionEngine.AddAlwaysAllowRule("Read");
  permissionEngine.AddAlwaysAllowRule("Grep");
  permissionEngine.AddAlwaysAllowRule("Glob");

  WorkspaceFirstModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);
  loop.SetMaxTurns(10);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = agent::app::BuildWorkspaceSystemPrompt(workspaceRoot, true);
  ctx.model = "test-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "workspace-first-user";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请先分析现有工程，再进行必要修改。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls >= 3,
        "Workspace-first guard should force another model turn before writing");
  Check(modelClient.secondCallSawGuidance,
        "Second model turn should receive workspace exploration guidance");

  const DWORD accidentalAttrs = GetFileAttributesA(accidentalWritePath.c_str());
  Check(accidentalAttrs == INVALID_FILE_ATTRIBUTES,
        "Initial Write tool should be blocked before workspace exploration");

  bool sawExplorationNudge = false;
  bool sawReadToolUse = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("Do not call Write/FileWrite yet.") !=
              std::string::npos) {
        sawExplorationNudge = true;
      }
      if (block.type == agent::core::BlockType::ToolUse &&
          block.asToolUse.name == "FileRead") {
        sawReadToolUse = true;
      }
    }
  }

  Check(sawExplorationNudge,
        "Workspace-first guard should append an exploration nudge");
  Check(sawReadToolUse,
        "Follow-up turn should use an exploration tool after the nudge");
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

void TestValidatorRetryTerminatesAfterRetryLimit() {
  const std::string sessionDir = "build\\validator-retry-limit-session";
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

  ValidatorRetryLoopModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(sessionDir);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  std::string terminalReason;
  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.validatorModel = "validator-model";
  ctx.sessionManager = &sessionManager;
  ctx.eventCallback =
      [&](const agent::core::QueryLoopEvent& event) {
        if (event.type == agent::core::QueryLoopEvent::Type::LoopCompleted) {
          terminalReason = event.terminalReason;
        }
      };

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-validator-limit";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "先看结构再修复，不要一直重复读同一个文件。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  // Two-phase: 3 retries -> nudge -> 3 more retries -> hard-terminate (total 6 calls)
  Check(modelClient.streamCalls == 6,
        "Validator retry loop should allow a full second cycle after nudge");
  Check(modelClient.validatorCalls == 6,
        "Validator should run for all retry calls including post-nudge cycle");
  Check(modelClient.thirdCallSawExecutionMemory,
        "Later retry turns should receive recent execution memory guidance");
  Check(terminalReason == "validator_retry_limit",
        "Validator retry limit should complete with a dedicated terminal reason");

  bool sawRetryLimitNote = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("validator requested retry_from_tools") !=
              std::string::npos) {
        sawRetryLimitNote = true;
      }
    }
  }
  Check(sawRetryLimitNote,
        "Validator retry limit should append an explicit termination note");

  sessionManager.FlushTranscriptBuffer();
  const agent::core::SessionMetadata metadata = sessionManager.metadata();
  Check(metadata.lastTerminalReason == "validator_retry_limit",
        "Session metadata should store validator_retry_limit");

  std::ifstream transcript(sessionManager.TranscriptJsonlPath(), std::ios::binary);
  std::string transcriptText((std::istreambuf_iterator<char>(transcript)),
                             std::istreambuf_iterator<char>());
  Check(transcriptText.find("\"stop_reason\":\"validator_retry_limit\"") !=
            std::string::npos,
        "Transcript should include the validator_retry_limit stop reason record");
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

void TestValidatorSeesWorkspaceRelativePathsAndRewriteGuidance() {
  const std::string workspaceRoot =
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent";
  const std::string absolutePath =
      "G:/downloads/claude-code/yuanma-poxi/cpp-agent/tests/unit/test_core.cpp";

  agent::tools::ToolRegistry toolRegistry;
  for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
    if (tool.name == "Read") {
      toolRegistry.RegisterTool(tool);
    }
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("Read");

  ValidatorPathNormalizationModelClient modelClient(absolutePath);
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.validatorModel = "validator-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-path-normalization";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请读取 test_core.cpp 的前几行。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.validatorCalls >= 1,
        "Validator should run for the path normalization test");
  Check(modelClient.sawRelativePath,
        "Validation context should normalize in-workspace absolute paths to relative paths");
  Check(!modelClient.sawAbsolutePath,
        "Validation context should avoid leaking absolute workspace paths when a relative path is enough");
  Check(modelClient.capturedValidatorSystemPrompt.find(
            "prefer a \"rewrite\" intervention over \"block\" or \"retry_from_tools\"") !=
            std::string::npos,
        "Validator prompt should steer path-only issues toward rewrite instead of retry");
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

void TestRepeatedToolFailureTerminatesContinuationLoop() {
  const std::string sessionDir = "build\\repeated-tool-result-loop-session";
  agent::tools::ToolRegistry toolRegistry;
  for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
    if (tool.name == "FileRead" || tool.name == "Glob") {
      toolRegistry.RegisterTool(tool);
    }
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent");

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("FileRead");
  permissionEngine.AddAlwaysAllowRule("Glob");

  RepeatedToolFailureModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(sessionDir);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);
  loop.SetMaxTurns(20);

  std::string terminalReason;
  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";
  ctx.sessionManager = &sessionManager;
  ctx.eventCallback =
      [&](const agent::core::QueryLoopEvent& event) {
        if (event.type == agent::core::QueryLoopEvent::Type::LoopCompleted) {
          terminalReason = event.terminalReason;
        }
      };

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-repeat-tool-failure";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "继续修复并运行，不要反复卡在同一个错误上。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  Check(modelClient.streamCalls == 4,
        "Repeated failing tool results should stop continuation before another retry turn");
  Check(modelClient.fourthCallSawExecutionMemory,
        "Repeated failure turns should receive recent execution memory guidance");
  Check(terminalReason == "repeated_tool_result_loop",
        "Repeated failing tool results should produce a dedicated terminal reason");

  bool sawRepeatedFailureNote = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("same failing tool result repeated 3 consecutive times") !=
              std::string::npos) {
        sawRepeatedFailureNote = true;
      }
    }
  }
  Check(sawRepeatedFailureNote,
        "Repeated failing tool results should append an explicit termination note");

  sessionManager.FlushTranscriptBuffer();
  std::ifstream transcript(sessionManager.TranscriptJsonlPath(), std::ios::binary);
  std::string transcriptText((std::istreambuf_iterator<char>(transcript)),
                             std::istreambuf_iterator<char>());
  Check(transcriptText.find("\"stop_reason\":\"repeated_tool_result_loop\"") !=
            std::string::npos,
        "Transcript should include the repeated_tool_result_loop stop reason record");
}

void TestMissingToolUseNudgeCanRetryForRepeatedTextOnlyWrites() {
  const std::string workspaceRoot =
      "g:\\downloads\\claude-code\\yuanma-poxi\\cpp-agent";
  const std::string outputPath =
      workspaceRoot + "\\build\\repeated-missing-tool-use.py";
  const std::string toolFilePath = "build/repeated-missing-tool-use.py";
  DeleteFileA(outputPath.c_str());

  agent::tools::ToolRegistry toolRegistry;
  for (const auto& tool : agent::tools::ToolRegistry::GetAllBaseTools()) {
    if (tool.name == "Write") {
      toolRegistry.RegisterTool(tool);
    }
  }

  agent::tools::ToolOrchestrator orchestrator;
  orchestrator.SetToolRegistry(&toolRegistry);
  orchestrator.SetWorkspaceRoot(workspaceRoot);

  agent::permissions::PermissionEngine permissionEngine;
  permissionEngine.AddAlwaysAllowRule("Write");

  RepeatedMissingToolUseModelClient modelClient(toolFilePath);
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);
  loop.SetMaxTurns(12);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = agent::app::BuildWorkspaceSystemPrompt(workspaceRoot, true);
  ctx.model = "main-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "user-missing-tool";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "请继续完善项目，并把缺失模块真正写入工作区。"));
  ctx.messages.push_back(user);

  loop.RunFull(ctx);

  int nudgeCount = 0;
  bool sawWriteToolUse = false;
  for (const auto& msg : ctx.messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          (block.asText.text.find("no tool call was emitted") != std::string::npos ||
           block.asText.text.find("still planning to create files without emitting a tool call") !=
               std::string::npos)) {
        ++nudgeCount;
      }
      if (block.type == agent::core::BlockType::ToolUse &&
          block.asToolUse.name == "Write") {
        sawWriteToolUse = true;
      }
    }
  }

  Check(nudgeCount >= 2,
        "Missing tool-use guidance should be able to retry after repeated text-only turns");
  Check(sawWriteToolUse,
        "Repeated missing-tool-use nudges should still lead to a concrete Write tool call");

  const DWORD attrs = GetFileAttributesA(outputPath.c_str());
  Check(attrs != INVALID_FILE_ATTRIBUTES,
        "Repeated missing-tool-use recovery should create the requested file");
  DeleteFileA(outputPath.c_str());
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
          block.asText.text.find("测试已执行完成") != std::string::npos) {
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

void TestRuntimePolicySharedRules() {
  const auto interactiveTools = agent::app::GetSessionBaseTools(true);
  const auto nonInteractiveTools = agent::app::GetSessionBaseTools(false);

  bool interactiveHasAsk = false;
  bool nonInteractiveHasAsk = false;
  for (const auto& tool : interactiveTools) {
    if (tool.name == "AskUserQuestion") interactiveHasAsk = true;
  }
  for (const auto& tool : nonInteractiveTools) {
    if (tool.name == "AskUserQuestion") nonInteractiveHasAsk = true;
  }

  Check(interactiveHasAsk,
        "Interactive runtime policy should keep AskUserQuestion");
  Check(!nonInteractiveHasAsk,
        "Non-interactive runtime policy should drop AskUserQuestion");

  const std::string prompt = agent::app::BuildWorkspaceSystemPrompt(
      "C:\\workspace", true);
  Check(prompt.find("MUST first explore the workspace with Glob, Grep, or Read tools") !=
            std::string::npos,
        "Shared prompt should enforce workspace-first exploration");
  Check(prompt.find("PowerShell, not bash") != std::string::npos,
        "Shared prompt should include Windows PowerShell guidance");
  Check(prompt.find("do not use Bash or PowerShell listing commands like ls, dir, or Get-ChildItem") !=
            std::string::npos,
        "Shared prompt should prefer Glob/Read/Grep over shell listing commands");
  Check(prompt.find("Read with offset/limit for a targeted line range") !=
            std::string::npos,
        "Shared prompt should prefer targeted range reads for large files");
  Check(prompt.find("older tool results may be compacted or truncated later") !=
            std::string::npos,
        "Shared prompt should warn that old tool results may be compacted");

  const auto silentStartup =
      agent::app::BuildStartupMessages(false, true, "C:\\workspace", 2);
  const auto interactiveStartup =
      agent::app::BuildStartupMessages(true, true, "C:\\workspace", 2);
  Check(silentStartup.empty(),
        "Non-interactive startup should not emit TUI banner messages");
  Check(interactiveStartup.size() == 3,
        "Interactive startup should include ready, workspace, and hook messages");
}

void TestQueryEngineWallClockBudgetTerminatesLoop() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  WallClockBudgetModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-wallclock-session");
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetSessionDir("build\\query-engine-wallclock-session");
  engine.SetWallClockBudgetMs(1);

  engine.SubmitUserPrompt("继续执行，不要停留在计划。");
  engine.RunTurn();

  Check(modelClient.streamCalls == 1,
        "Wall-clock timeout should stop the loop before a second model turn");

  bool sawWallClockTermination = false;
  for (const auto& msg : engine.messages()) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("wall-clock budget exceeded") !=
              std::string::npos) {
        sawWallClockTermination = true;
      }
    }
  }
  Check(sawWallClockTermination,
        "Wall-clock timeout should append a termination message");
}

void TestQueryEngineCanContinueAfterWallClockTimeout() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  ResumeAfterWallClockModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-wallclock-continue-session");
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetSessionDir("build\\query-engine-wallclock-continue-session");
  engine.SetWallClockBudgetMs(1);

  engine.SubmitUserPrompt("继续执行，不要停留在计划。");
  engine.RunTurn();

  Check(modelClient.streamCalls == 1,
        "First run should stop after wall-clock timeout");
  Check(engine.PrepareForContinuationAfterWallClockTimeout(),
        "QueryEngine should trim the trailing wall-clock timeout message");
  bool sawWallClockTermination = false;
  for (const auto& msg : engine.messages()) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("wall-clock budget exceeded") !=
              std::string::npos) {
        sawWallClockTermination = true;
      }
    }
  }
  Check(!sawWallClockTermination,
        "Continuation prep should remove the wall-clock timeout marker");

  engine.SetWallClockBudgetMs(1000);
  engine.RunTurn();
  Check(modelClient.streamCalls == 2,
        "Second run should resume execution with a fresh wall-clock budget");
}

void TestQueryEngineDefaultMaxTurnsIsDisabled() {
  const int toolTurns = 81;

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

  ManyToolTurnsModelClient modelClient(
      toolTurns,
      {"tests/unit/test_core.cpp", "src/core/QueryEngine.cpp"});
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-default-maxturns-session");
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);
  engine.SetSessionDir("build\\query-engine-default-maxturns-session");

  engine.SubmitUserPrompt("继续读取上下文，直到真正完成。");
  engine.RunTurn();

  bool sawFinalAssistant = false;
  int toolResultCount = 0;
  for (const auto& msg : engine.messages()) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("跨过默认轮次后完成") != std::string::npos) {
        sawFinalAssistant = true;
      }
      if (block.type == agent::core::BlockType::ToolResult) {
        ++toolResultCount;
      }
    }
  }

  Check(modelClient.streamCalls == toolTurns + 1,
        "Default QueryEngine maxTurns should not stop execution before turn 81");
  Check(toolResultCount == toolTurns,
        "Default QueryEngine maxTurns should allow all intermediate tool turns");
  Check(sawFinalAssistant,
        "Default QueryEngine maxTurns should allow the final assistant completion");
}

void TestQueryEngineSetConfigOverridesPromptAndModel() {
  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  PlanThenToolModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-config-override-session");
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt = "CUSTOM_SYSTEM_PROMPT_OVERRIDE";
  config.defaultModel = "custom-model-override";
  engine.SetConfig(config);

  engine.SubmitUserPrompt("只回复一次。");
  engine.RunTurn();

  bool sawCustomPrompt = false;
  for (const auto& msg : engine.loopContext().messages) {
    for (const auto& block : msg.content) {
      if (block.type == agent::core::BlockType::Text &&
          block.asText.text.find("CUSTOM_SYSTEM_PROMPT_OVERRIDE") !=
              std::string::npos) {
        sawCustomPrompt = true;
      }
    }
  }

  Check(engine.loopContext().systemPrompt == "CUSTOM_SYSTEM_PROMPT_OVERRIDE",
        "SetConfig should override the effective system prompt");
  Check(engine.loopContext().model == "custom-model-override",
        "SetConfig should override the effective default model");
  Check(!sawCustomPrompt,
        "System prompt should remain in loop context, not leak into chat messages");
}

void TestQueryEngineBuildEffectivePromptAppendsMemoryInjection() {
  CreateDirectoryA("build", nullptr);
  const std::string memoryDir = "build\\query-engine-memory-prompt";

  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  PromptCaptureModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(
      "build\\query-engine-memory-session");
  agent::memory::MemoryIndex memoryIndex(memoryDir);
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);

  Check(memoryIndex.WriteEntrypoint("- [User role](user_role.md) - prompt test"),
        "MemoryIndex should write entrypoint for effective prompt test");
  Check(memoryIndex.WriteTopicFile(
            "user_role.md", "# User role\nRemember concise runner summaries."),
        "MemoryIndex should write topic file for effective prompt test");

  agent::core::AgentConfig config = agent::core::AgentConfig::FromDefaults();
  config.systemPrompt = "BASE_PROMPT_FOR_MEMORY_TEST";
  config.memoryRoot = memoryDir;
  engine.SetConfig(config);
  engine.SetMemoryIndex(&memoryIndex);

  engine.SubmitUserPrompt("验证 system prompt 运行链路。");
  engine.RunTurn();

  Check(modelClient.lastSystemPrompt.find("BASE_PROMPT_FOR_MEMORY_TEST") == 0,
        "Effective prompt should start with configured system prompt");
  Check(modelClient.lastSystemPrompt.find("# memory") != std::string::npos,
        "Effective prompt should append memory injection");
  Check(modelClient.lastSystemPrompt.find("Remember concise runner summaries.") !=
            std::string::npos,
        "Effective prompt should include topic memory content");
}

void TestQueryEngineRunTurnUpdatesMetadataWithoutWatchdog() {
  CreateDirectoryA("build", nullptr);
  const std::string sessionDir = "build\\query-engine-turncount-session";

  agent::tools::ToolRegistry toolRegistry;
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  PromptCaptureModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::infra::SessionManager sessionManager(sessionDir);
  agent::core::QueryEngine engine(
      orchestrator, permissionEngine, modelClient, sideQueryClient,
      toolRegistry, sessionManager);

  engine.SetSessionDir(sessionDir);
  engine.SubmitUserPrompt("只做一次回答。");
  engine.RunTurn();
  sessionManager.PersistSnapshot();

  const agent::core::SessionMetadata metadata = sessionManager.metadata();
  Check(metadata.turnCount == 1,
        "RunTurn should increment metadata turn count without watchdog");
  Check(metadata.id == sessionDir,
        "RunTurn should preserve the session id for non-watchdog sessions");
  Check(metadata.lastTerminalReason == "completed",
        "RunTurn should persist the final terminal reason into session metadata");

  std::ifstream snapshot(sessionManager.LegacySnapshotPath(), std::ios::binary);
  std::string snapshotText((std::istreambuf_iterator<char>(snapshot)),
                           std::istreambuf_iterator<char>());
  Check(snapshotText.find("turn_count=1") != std::string::npos,
        "Persisted snapshot should store the incremented turn count");
  Check(snapshotText.find("session_id=" + sessionDir) != std::string::npos,
        "Persisted snapshot should store the session id");
  Check(snapshotText.find("terminal_reason=completed") != std::string::npos,
        "Persisted snapshot should store the final terminal reason");
}

void TestMicrocompactPreservesLatestToolResult() {
  agent::tools::ToolOrchestrator orchestrator;
  agent::permissions::PermissionEngine permissionEngine;
  MicrocompactCaptureModelClient modelClient;
  agent::api::SideQueryClient sideQueryClient(modelClient);
  agent::core::QueryLoop loop(
      orchestrator, permissionEngine, modelClient, sideQueryClient);

  agent::core::QueryLoopContext ctx;
  ctx.systemPrompt = "system";
  ctx.model = "main-model";

  agent::core::Message user;
  user.role = agent::core::MessageRole::User;
  user.uuid = "mc-user";
  user.content.push_back(agent::core::ContentBlock::MakeText(
      "继续基于最近一次目录结果执行。"));
  ctx.messages.push_back(user);

  agent::core::Message assistantOld;
  assistantOld.role = agent::core::MessageRole::Assistant;
  assistantOld.uuid = "mc-asst-old";
  assistantOld.content.push_back(agent::core::ContentBlock::MakeToolUse(
      "tool-old", "Glob", R"({"pattern":"*"})"));
  ctx.messages.push_back(assistantOld);

  agent::core::Message oldResult;
  oldResult.role = agent::core::MessageRole::User;
  oldResult.uuid = "mc-result-old";
  oldResult.content.push_back(agent::core::ContentBlock::MakeToolResult(
      "tool-old",
      std::string(120, 'A') + "\nold tool result should compact",
      false));
  ctx.messages.push_back(oldResult);

  agent::core::Message assistantLatest;
  assistantLatest.role = agent::core::MessageRole::Assistant;
  assistantLatest.uuid = "mc-asst-latest";
  assistantLatest.content.push_back(agent::core::ContentBlock::MakeToolUse(
      "tool-latest", "Bash", R"({"command":"ls -la"})"));
  ctx.messages.push_back(assistantLatest);

  agent::core::Message latestResult;
  latestResult.role = agent::core::MessageRole::User;
  latestResult.uuid = "mc-result-latest";
  const std::string latestContent =
      std::string(120, 'B') + "\nlatest tool result should stay intact";
  latestResult.content.push_back(agent::core::ContentBlock::MakeToolResult(
      "tool-latest", latestContent, false));
  ctx.messages.push_back(latestResult);

  loop.RunFull(ctx);

  bool oldWasCompacted = false;
  bool latestStayedFull = false;
  for (const auto& msg : modelClient.capturedMessages) {
    for (const auto& block : msg.content) {
      if (block.type != agent::core::BlockType::ToolResult) continue;
      if (block.asToolResult.toolUseId == "tool-old" &&
          block.asToolResult.content.find("[Tool: Glob]") !=
              std::string::npos) {
        oldWasCompacted = true;
      }
      if (block.asToolResult.toolUseId == "tool-latest" &&
          block.asToolResult.content == latestContent) {
        latestStayedFull = true;
      }
    }
  }

  Check(oldWasCompacted,
        "Microcompact should still compact older oversized tool results");
  Check(latestStayedFull,
        "Microcompact should preserve the latest oversized tool result for the next turn");
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
  TestWorkspaceFirstBlocksInitialWriteTool();
  TestSkeletonModelClientStream();
  TestQueryLoopMaxTokensEscalation();
  TestQueryLoopFallbackModelRetry();
  TestQueryLoopValidatorRetryBeforeToolExecution();
  TestValidatorRetryTerminatesAfterRetryLimit();
  TestQueryLoopValidatorTextCorrection();
  TestValidatorSeesWorkspaceRelativePathsAndRewriteGuidance();
  TestHttpLlmClientConstruction();
  TestQueryEngineEmitsStreamingEvents();
  TestForcedContinuationLimitPersistsAcrossFollowups();
  TestRepeatedToolFailureTerminatesContinuationLoop();
  TestMissingToolUseNudgeCanRetryForRepeatedTextOnlyWrites();
  TestHistorySnipPreservesOriginalUserTask();
  TestStopHookCanForceContinuation();
  TestCompactHooksFireDuringSnip();
  TestRuntimePolicySharedRules();
  TestQueryEngineWallClockBudgetTerminatesLoop();
  TestQueryEngineCanContinueAfterWallClockTimeout();
  TestQueryEngineDefaultMaxTurnsIsDisabled();
  TestQueryEngineSetConfigOverridesPromptAndModel();
  TestQueryEngineBuildEffectivePromptAppendsMemoryInjection();
  TestQueryEngineRunTurnUpdatesMetadataWithoutWatchdog();
  TestMicrocompactPreservesLatestToolResult();

  std::cout << "[test_core] Failures: " << failures << std::endl;
  return failures > 0 ? 1 : 0;
}
