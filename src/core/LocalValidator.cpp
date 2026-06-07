#include "core/LocalValidator.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include "third_party/nlohmann_json.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace agent {
namespace core {

using json = nlohmann::json;

// ============================================================================
// Constants ? aligned with local-ace validation/index.ts
// ============================================================================

// Commands that are Unix-only and will fail on Windows PowerShell
static const std::vector<std::string> kUnixOnlyCommands = {
  "grep ", "head -", "tail -", "sed ", "awk ", "xargs",
  "| head", "| tail", "| grep", "| sed", "| awk",
  "2>/dev/null", ">/dev/null", "/dev/null",
};

// Tool names that mutate the workspace (mirrors MUTATING_TOOL_NAME_PATTERN)
static const std::vector<std::string> kMutatingToolNames = {
  "Write", "FileWrite", "WriteFile", "FileEdit", "MultiEdit", "NotebookEdit"
};

// User goal patterns for direct text response (mirrors requiresDirectTextResponse)
static const std::vector<std::string> kDirectAnswerPatterns = {
  "directly", "just tell", "just answer", "only respond",
  "don't write", "do not write", "do not create", "without files",
  "no files", "no file", "in chat", "in the chat",
};

// User goal patterns for forbidding file operations (mirrors forbidsFileOperations)
static const std::vector<std::string> kNoFileOpsPatterns = {
  "don't create files", "do not create files", "without creating files",
  "no file creation", "without writing files", "read-only",
  "don't save", "do not save", "without saving",
};

// User goal patterns for HTML/CSS only (mirrors requiresHtmlCssOnly)
static const std::vector<std::string> kHtmlCssOnlyPatterns = {
  "html", "css", "html file", "css file", "webpage", "web page",
};

// User goal patterns for forbidding JavaScript (mirrors forbidsJavaScript)
static const std::vector<std::string> kNoJsPatterns = {
  "no javascript", "without javascript", "no js", "without js",
  "pure html", "static html", "plain html",
};

// User goal patterns for forbidding Markdown (mirrors forbidsMarkdown)
static const std::vector<std::string> kNoMarkdownPatterns = {
  "no markdown", "without markdown", "plain text", "not markdown",
  "don't use markdown", "do not use markdown",
};

// ============================================================================
// Utility functions
// ============================================================================

static bool ToolNameMatchesCaseInsensitive(const std::string& toolName,
                                            const std::vector<std::string>& patterns) {
  std::string lower = toolName;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  for (const auto& p : patterns) {
    std::string plower = p;
    std::transform(plower.begin(), plower.end(), plower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (lower == plower) return true;
  }
  return false;
}

static std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char ch) {
                   return static_cast<char>(std::tolower(ch));
                 });
  return value;
}

static bool GoalContainsAnyPattern(const std::string& lowerGoal,
                                    const std::vector<std::string>& patterns) {
  for (const auto& p : patterns) {
    if (lowerGoal.find(p) != std::string::npos) return true;
  }
  return false;
}

// ============================================================================
// Public API implementations
// ============================================================================

ValidationContext BuildValidationContext(
    const std::vector<Message>& messages,
    const std::vector<Message>& assistantMessages,
    const std::vector<ContentBlock>& toolUseBlocks,
    const std::string& stopReason,
    ValidationMode mode) {
  ValidationContext ctx;
  ctx.mode = mode;
  ctx.stopReason = stopReason;
  ctx.toolUseBlocks = toolUseBlocks;
  ctx.originalUserGoal = ExtractOriginalUserGoal(messages);
  ctx.assistantText = ExtractAssistantText(assistantMessages);
  return ctx;
}

std::string ExtractOriginalUserGoal(const std::vector<Message>& messages) {
  // Walk messages from end to start, find the last non-meta user message
  for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
    if (it->role != MessageRole::User) continue;
    if (it->isMeta) continue;
    for (const auto& block : it->content) {
      if (block.type == BlockType::Text && !block.asText.text.empty()) {
        return block.asText.text;
      }
    }
  }
  return std::string();
}

std::string ExtractAssistantText(const std::vector<Message>& assistantMessages) {
  std::ostringstream text;
  for (const auto& msg : assistantMessages) {
    for (const auto& block : msg.content) {
      if (block.type == BlockType::Text && !block.asText.text.empty()) {
        if (!text.str().empty()) text << "\n";
        text << block.asText.text;
      }
    }
  }
  return text.str();
}

bool RequiresDirectTextResponse(const std::string& goal) {
  if (goal.empty()) return false;
  std::string lower = ToLower(goal);
  return GoalContainsAnyPattern(lower, kDirectAnswerPatterns);
}

bool ForbidsFileOperations(const std::string& goal) {
  if (goal.empty()) return false;
  std::string lower = ToLower(goal);
  return GoalContainsAnyPattern(lower, kNoFileOpsPatterns);
}

bool RequiresHtmlCssOnly(const std::string& goal) {
  if (goal.empty()) return false;
  std::string lower = ToLower(goal);
  // Must contain both "html" and no-js patterns
  bool wantsHtml = GoalContainsAnyPattern(lower, kHtmlCssOnlyPatterns);
  bool noJs = GoalContainsAnyPattern(lower, kNoJsPatterns);
  return wantsHtml && noJs;
}

bool ForbidsJavaScript(const std::string& goal) {
  if (goal.empty()) return false;
  std::string lower = ToLower(goal);
  return GoalContainsAnyPattern(lower, kNoJsPatterns);
}

bool ForbidsMarkdown(const std::string& goal) {
  if (goal.empty()) return false;
  std::string lower = ToLower(goal);
  return GoalContainsAnyPattern(lower, kNoMarkdownPatterns);
}

std::vector<std::string> BuildCompactConstraints(const std::string& goal) {
  std::vector<std::string> constraints;
  if (ForbidsFileOperations(goal)) {
    constraints.push_back("NO_FILE_OPERATIONS");
  }
  if (ForbidsJavaScript(goal)) {
    constraints.push_back("NO_JAVASCRIPT");
  }
  if (ForbidsMarkdown(goal)) {
    constraints.push_back("NO_MARKDOWN");
  }
  if (RequiresDirectTextResponse(goal)) {
    constraints.push_back("DIRECT_TEXT_ONLY");
  }
  return constraints;
}

// ============================================================================
// Local validation ? aligned with local-ace runLocalToolValidation
// ============================================================================

LocalValidationResult RunLocalToolValidation(const ValidationContext& context) {
  LocalValidationResult result;

  if (context.toolUseBlocks.empty()) return result;

  // Rule 1: Check for Unix commands on Windows (cpp-agent specific)
  CheckUnixCommandsOnWindows(context.toolUseBlocks, result);

  // Rule 2: Check if user wants direct text response (mirrors local-ace)
  const bool shouldAvoidMutatingTools =
      RequiresDirectTextResponse(context.originalUserGoal) ||
      ForbidsFileOperations(context.originalUserGoal);

  if (shouldAvoidMutatingTools) {
    std::vector<const ContentBlock*> blockedTools;
    for (const auto& block : context.toolUseBlocks) {
      if (block.type != BlockType::ToolUse) continue;
      if (ToolNameMatchesCaseInsensitive(block.asToolUse.name, kMutatingToolNames)) {
        blockedTools.push_back(&block);
      }
    }

    if (!blockedTools.empty()) {
      for (const auto* block : blockedTools) {
        result.ruleHits.push_back({
          "direct_text_required", "block",
          "Tool " + block->asToolUse.name +
          " is unnecessary: user asked for a direct response."
        });

        ValidationToolIntervention intervention;
        intervention.toolUseId = block->asToolUse.id;
        intervention.action = "block";
        intervention.blockGuidance =
            "Return the requested result directly in the chat. "
            "Do not create or modify files for this task.";

        if (!result.immediateResult) {
          result.immediateResult = ValidationResult{};
        }
        result.immediateResult->toolInterventions.push_back(intervention);
      }

      if (result.immediateResult) {
        result.immediateResult->finalResponseAction = "retry_from_tools";
        result.immediateResult->retryGuidance =
            "Return the requested result directly in the chat. "
            "Do not create or modify files unless the user "
            "explicitly asks for file operations.";
      }
    }
  }

  // P1-3: Check for TodoWrite with all completed tasks but no verification step.
  // This mirrors local-ace's verificationNudgeNeeded mechanism.
  for (const auto& block : context.toolUseBlocks) {
    if (block.type != BlockType::ToolUse) continue;
    if (block.asToolUse.name != "TodoWrite") continue;

    // Parse the todos from the input
    try {
      auto j = json::parse(block.asToolUse.inputJson);
      auto todos = j.value("todos", json::array());
      if (!todos.is_array() || todos.size() < 3) continue;

      bool allCompleted = true;
      bool hasVerificationStep = false;
      for (const auto& todo : todos) {
        std::string status = todo.value("status", std::string());
        std::string content = todo.value("content",
            todo.value("subject", std::string()));
        if (status != "completed") {
          allCompleted = false;
        }
        // Check for verification-related keywords
        std::string lowerContent = content;
        std::transform(lowerContent.begin(), lowerContent.end(),
                       lowerContent.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lowerContent.find("verif") != std::string::npos ||
            lowerContent.find("test") != std::string::npos ||
            lowerContent.find("run") != std::string::npos ||
            lowerContent.find("check") != std::string::npos ||
            lowerContent.find("build") != std::string::npos) {
          hasVerificationStep = true;
        }
      }

      if (allCompleted && !hasVerificationStep) {
        result.ruleHits.push_back({
          "no_verification_step", "warn",
          "All tasks completed but none involves verification (run/test/check/build). "
          "Consider adding a verification step before reporting completion."
        });
        // Note: We only warn, not block. The actual nudge is handled by
        // ExecuteTodoWrite in ToolOrchestrator.cpp.
      }
    } catch (...) {
      continue;
    }
  }

  return result;
}

LocalValidationResult RunLocalFinalTextValidation(const ValidationContext& context) {
  LocalValidationResult result;

  if (context.assistantText.empty()) return result;

  const std::string& text = context.assistantText;

  // Rule: Check for JavaScript in HTML-only responses
  if (ForbidsJavaScript(context.originalUserGoal)) {
    // Check for script tags, event handlers, JS patterns
    bool hasJS =
        text.find("<script") != std::string::npos ||
        text.find("javascript:") != std::string::npos ||
        text.find("onclick=") != std::string::npos ||
        text.find("addEventListener") != std::string::npos ||
        text.find("function(") != std::string::npos;

    if (hasJS) {
      result.ruleHits.push_back({
        "js_forbidden", "block",
        "JavaScript detected in response but user forbids JavaScript"
      });

      result.immediateResult = ValidationResult{};
      result.immediateResult->finalResponseAction = "retry_from_tools";
      result.immediateResult->retryGuidance =
          "JavaScript was detected in your response. "
          "The user explicitly asked for NO JavaScript. "
          "Rewrite using only HTML and CSS.";
    }
  }

  // Rule: Check for Markdown in plain-text responses
  if (ForbidsMarkdown(context.originalUserGoal)) {
    bool hasMarkdown =
        text.find("```") != std::string::npos ||
        text.find("**") != std::string::npos ||
        text.find("##") != std::string::npos ||
        text.find("###") != std::string::npos ||
        text.find("- ") != std::string::npos ||
        text.find("* ") != std::string::npos;

    if (hasMarkdown) {
      result.ruleHits.push_back({
        "markdown_forbidden", "fix",
        "Markdown detected in response but user wants plain text"
      });

      if (!result.immediateResult) {
        result.immediateResult = ValidationResult{};
      }
      result.immediateResult->finalResponseAction = "retry_from_tools";
      result.immediateResult->retryGuidance =
          "Formatting was detected in your response. "
          "The user asked for PLAIN TEXT without Markdown. "
          "Remove all formatting and use plain text only.";
    }
  }

  return result;
}

// ============================================================================
// isValidationEnabled ? mirrors local-ace validation/index.ts:10
// ============================================================================
bool IsValidationEnabled() {
#ifdef _WIN32
  char buffer[64] = {0};
  DWORD len = GetEnvironmentVariableA("AGENT_DISABLE_VALIDATION", buffer, sizeof(buffer));
  if (len > 0 && len < sizeof(buffer)) {
    std::string val(buffer, len);
    if (val == "1" || val == "true" || val == "TRUE" ||
        val == "yes" || val == "YES" || val == "on" || val == "ON") {
      return false;
    }
  }
#else
  const char* val = std::getenv("AGENT_DISABLE_VALIDATION");
  if (val) {
    std::string s(val);
    if (s == "1" || s == "true" || s == "TRUE" ||
        s == "yes" || s == "YES" || s == "on" || s == "ON") {
      return false;
    }
  }
#endif
  return true;  // Default: enabled
}

// ============================================================================
// Unix command detection (cpp-agent specific, not in local-ace)
// ============================================================================

void CheckUnixCommandsOnWindows(
    const std::vector<ContentBlock>& toolUseBlocks,
    LocalValidationResult& result) {
  for (const auto& block : toolUseBlocks) {
    if (block.type != BlockType::ToolUse) continue;
    if (block.asToolUse.name != "Bash") continue;

    std::string command;
    try {
      auto j = json::parse(block.asToolUse.inputJson);
      if (j.contains("command") && j["command"].is_string())
        command = j["command"].get<std::string>();
    } catch (...) { continue; }
    if (command.empty()) continue;

    for (const auto& pattern : kUnixOnlyCommands) {
      if (command.find(pattern) == std::string::npos) continue;

      result.ruleHits.push_back({
        "unix_cmd_on_windows", "fix",
        "Bash command uses Unix-only '" + pattern +
        "' which fails on Windows PowerShell."
      });

      // Note: command rewriting is now handled by ToolOrchestrator::NormalizeWindowsShellCommand
      // This validator only records the detection for logging/monitoring.
      // We do NOT inject retry_from_tools here - the ToolOrchestrator handles command conversion.

      break;
    }
  }
}

// ============================================================================
// Apply local validation result ? aligned with local-ace
// ============================================================================

void ApplyLocalValidationResult(
    const LocalValidationResult& local,
    QueryLoopInternalState& state) {
  if (!local.immediateResult) return;
  const auto& vr = *local.immediateResult;

  if (!vr.toolInterventions.empty()) {
    std::vector<ContentBlock> rewritten;
    std::set<std::string> blockedIds;
    std::map<std::string, std::string> blockGuidance;

    for (const auto& block : state.toolUseBlocks) {
      bool matched = false;
      for (const auto& ti : vr.toolInterventions) {
        if (block.asToolUse.id != ti.toolUseId) continue;
        matched = true;
        if (ti.action == "rewrite") {
          ContentBlock rb = block;
          if (!ti.correctedName.empty())
            rb.asToolUse.name = ti.correctedName;
          if (!ti.correctedInputJson.empty())
            rb.asToolUse.inputJson = ti.correctedInputJson;
          rewritten.push_back(rb);
        } else if (ti.action == "block") {
          blockedIds.insert(ti.toolUseId);
          blockGuidance[ti.toolUseId] =
              ti.blockGuidance.empty() ? "unsafe" : ti.blockGuidance;
        }
        break;
      }
      if (!matched) rewritten.push_back(block);
    }
    state.toolUseBlocks = rewritten;

    for (const auto& [blockId, guidance] : blockGuidance) {
      Message synthetic;
      synthetic.role = MessageRole::User;
      synthetic.uuid = "local-rule-blocked-" + blockId;
      synthetic.isMeta = true;
      synthetic.content.push_back(ContentBlock::MakeToolResult(
          blockId,
          "Tool call blocked by local rule: " + guidance,
          true));
      state.toolResultMessages.push_back(synthetic);
    }
    if (state.toolUseBlocks.empty() && !state.toolResultMessages.empty()) {
      state.forceContinuation = true;
      state.forceContinuationReason = "local_rule_blocked_tools";
    }
  }

  if (vr.finalResponseAction == "retry_from_tools") {
    const std::string retryGuidance =
        vr.retryGuidance.empty() ? "Retry from tools." : vr.retryGuidance;
    state.validatorRequestedRetry = true;
    ++state.validatorRetryCount;
    state.lastValidatorGuidance = "[local-rule] " + retryGuidance;
    state.toolUseBlocks.clear();
    Message guidance;
    guidance.role = MessageRole::User;
    guidance.uuid = "local-validator-retry";
    guidance.isMeta = true;
    guidance.content.push_back(ContentBlock::MakeText(
        "[Local Validator] " + retryGuidance));
    state.pendingFollowupMessages.push_back(guidance);
  }
}

}  // namespace core
}  // namespace agent
