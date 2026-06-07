#pragma once

#include "core/AgentTypes.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

namespace agent {
namespace core {

enum class PermissionMode {
  Default,
  Auto,
  BypassPermissions,
  Plan,
  AcceptEdits,
  DontAsk
};

struct LlmConfig {
  std::string apiEndpoint;
  std::string apiKey;
  std::string mainModel;
  std::string validatorModel;
  std::string fallbackModel;
  int connectTimeoutMs = 30000;
  int requestTimeoutMs = 120000;
  // Model-specific overrides (0 = use model-family defaults)
  int contextWindowOverride = 0;
  int maxOutputTokensOverride = 0;
};

struct DenialTrackingState {
  int consecutive = 0;
  int total = 0;
  int maxConsecutive = 3;
  int maxTotal = 20;

  bool IsCircuitBroken() const {
    return consecutive >= maxConsecutive || total >= maxTotal;
  }

  void RecordDenial() {
    ++consecutive;
    ++total;
  }

  void RecordApproval() { consecutive = 0; }

  void Reset() {
    consecutive = 0;
    total = 0;
  }
};

struct AgentConfig {
  std::string memoryRoot;
  std::string sessionDir;
  std::string logDir;
  std::string defaultModel = "default-model";
  std::string systemPrompt =
      "You are a coding agent running inside a local project workspace. "
      "Use the available tools to inspect files, create files, and modify the "
      "workspace when the user's request implies real project changes. "
      "Prefer Read/Write-style tool calls over pasting large code blobs into "
      "chat when the result should exist as a real file on disk. "
      "Do not reveal chain-of-thought or write 'thinking process' in the "
      "final answer. Be concise, action-oriented, and continue the turn after "
      "tool results until the requested file or change is actually completed.\n\n"
      "CODE GENERATION CONVENTIONS:\n"
      "- When generating a multi-module project with an orchestration script "
      "(main.py, run.py, app.py, etc.), you MUST read the actual class "
      "definitions from all modules you've created and use the EXACT method "
      "names and signatures. Do NOT guess or invent method names.\n"
      "- After writing all modules, before running the code, verify API "
      "consistency: read each module's class definitions, check that main.py "
      "calls match actual methods, and fix any mismatches.\n"
      "- When you encounter an AttributeError, NameError, ImportError, or "
      "TypeError at runtime, READ the relevant source file(s) to understand "
      "the actual API before attempting a fix. Guessing produces incorrect "
      "fixes and causes repeated failures.\n"
      "- The development workflow is: Write modules -> Read each module to "
      "verify APIs -> Fix mismatches -> Run -> Verify output -> Fix errors.\n"
      "- Always run the code after writing it to verify it works. Never mark "
      "a task as completed until the code runs without errors and produces "
      "expected output.";
  // Qwen/Gemma-adapted system prompt (Chinese-friendly, simpler structure)
  std::string systemPromptQwen =
      "你是一个在本地项目工作区中运行的编程助手。"
      "使用可用工具来检查文件、创建文件和修改工作区。"
      "优先使用 Read/Write 工具调用来操作文件，而不是在聊天中粘贴大段代码。"
      "不要在最终回答中暴露思考过程。保持简洁、面向行动，"
      "在工具结果返回后继续完成任务，直到所请求的文件或更改实际完成。"
      "使用工具时，确保 JSON 参数格式正确。\n\n"
      "代码生成规范：\n"
      "- 生成多模块项目时（带有 main.py/run.py 等入口脚本），必须先读取所有已"
      "生成模块的类定义，使用精确的方法名和签名。不要猜测或捏造方法名。\n"
      "- 写完所有模块后、运行前，验证 API 一致性：读取每个模块的类定义，检查 "
      "main.py 中的调用是否正确，修复所有不匹配。\n"
      "- 遇到 AttributeError/NameError/ImportError/TypeError 时，先读取相关源码"
      "文件了解实际 API，再修复。猜测会导致错误的修复和反复失败。\n"
      "- 开发流程：写模块 -> 读模块验证API -> 修复不匹配 -> 运行 -> 验证输出 "
      "-> 修复错误。\n"
      "- 代码写完后必须运行验证。代码无错误且输出符合预期才标记任务完成。";
  std::string systemPromptGemma =
      "You are a coding assistant in a local project workspace. "
      "Use the provided tools (Read, Write, Grep, Glob, Bash) to inspect and modify files. "
      "Always use tool calls instead of pasting code in chat. "
      "Do not show your thinking process. Be concise and action-oriented. "
      "After receiving tool results, continue working until the task is complete. "
      "Ensure all tool call JSON arguments are valid.\n\n"
      "CODE GENERATION CONVENTIONS:\n"
      "- When generating multi-module projects with an entry point script, "
      "read the actual class definitions from all modules and use EXACT method "
      "names and signatures. Never guess method names.\n"
      "- After writing all modules, verify API consistency before running: "
      "read each module's class definitions, check main.py calls match.\n"
      "- When encountering AttributeError/NameError/ImportError/TypeError, "
      "READ the relevant source files to understand the actual API before "
      "attempting a fix. Guessing leads to incorrect fixes and repeated failures.\n"
      "- Workflow: Write modules -> Read modules to verify APIs -> Fix "
      "mismatches -> Run -> Verify output -> Fix errors.\n"
      "- Always run code after writing it. Only mark tasks as completed when "
      "the code runs without errors and produces expected output.";
  int maxToolUseConcurrency = 10;
  int perMessageBudgetLimit = 600000;
  int contextWindow = 200000;
  int autocompactBufferTokens = 13000;

  bool autoCompactEnabled = true;
  bool reactiveCompactEnabled = true;
  bool contextCollapseEnabled = true;
  bool historySnipEnabled = true;
  bool cachedMicrocompactEnabled = true;
  bool validatorEnabled = false;
  bool streamingToolExecutionEnabled = true;
  bool failClosedGate = true;
  bool autoModeEnabled = false;
  PermissionMode permissionMode = PermissionMode::Default;

  // Get the effective system prompt for a given model
  std::string GetEffectiveSystemPrompt(const std::string& model) const {
    if (!systemPrompt.empty() && systemPrompt != AgentConfig().systemPrompt) {
      return systemPrompt;  // User-overridden
    }
    ModelFamily fam = DetectModelFamily(model);
    if (fam == ModelFamily::Qwen) return systemPromptQwen;
    if (fam == ModelFamily::Gemma) return systemPromptGemma;
    return systemPrompt;
  }

  static AgentConfig FromDefaults();
};

struct AbortError {
  std::string message;
};

struct FallbackTriggeredError {
  std::string message;
};

struct SessionMetadata {
  std::string id;
  std::string startTime;
  int turnCount = 0;
  bool aborted = false;
  std::string lastTerminalReason;
};

}  // namespace core
}  // namespace agent
