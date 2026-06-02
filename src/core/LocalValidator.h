#pragma once

#include "core/AgentTypes.h"
#include "core/QueryEngine.h"
#include "core/QueryLoop.h"

#include <optional>
#include <string>
#include <vector>

namespace agent {
namespace core {

// ============================================================================
// Types aligned with local-ace validation/index.ts
// ============================================================================

// Mirrors local-ace ValidationMode
enum class ValidationMode {
  ToolUse,     // Validating tool use blocks
  FinalText,   // Validating final assistant text
};

// Mirrors local-ace ValidationRuleHit
struct LocalValidationHit {
  std::string ruleId;
  std::string severity;  // "block", "fix", "warn"
  std::string message;
};

// Mirrors local-ace ValidationContext
struct ValidationContext {
  ValidationMode mode = ValidationMode::ToolUse;
  std::string originalUserGoal;
  std::string assistantText;
  std::vector<ContentBlock> toolUseBlocks;
  std::string stopReason;
  int turnCount = 0;
  int maxOutputTokensOverride = 0;
};

// Result from local validation (no LLM)
struct LocalValidationResult {
  std::vector<LocalValidationHit> ruleHits;
  std::optional<ValidationResult> immediateResult;
};

// ============================================================================
// Public API ? aligned with local-ace validateAssistantDraft
// ============================================================================

// Build validation context from messages (mirrors local-ace buildValidationContext)
ValidationContext BuildValidationContext(
    const std::vector<Message>& messages,
    const std::vector<Message>& assistantMessages,
    const std::vector<ContentBlock>& toolUseBlocks,
    const std::string& stopReason,
    ValidationMode mode);

// Run local tool validation (mirrors local-ace runLocalToolValidation)
LocalValidationResult RunLocalToolValidation(const ValidationContext& context);

// Run local final text validation (mirrors local-ace runLocalFinalTextValidation)
LocalValidationResult RunLocalFinalTextValidation(const ValidationContext& context);

// Apply local validation result to the query loop state
void ApplyLocalValidationResult(
    const LocalValidationResult& local,
    QueryLoopInternalState& state);

// ============================================================================
// Utility functions (exposed for testing)
// ============================================================================

// Extract the original user goal from messages (mirrors local-ace extractOriginalUserGoal)
std::string ExtractOriginalUserGoal(const std::vector<Message>& messages);

// Extract assistant text from assistant messages (mirrors local-ace extractAssistantText)
std::string ExtractAssistantText(const std::vector<Message>& assistantMessages);

// Check if user goal requires a direct text response (mirrors local-ace requiresDirectTextResponse)
bool RequiresDirectTextResponse(const std::string& goal);

// Check if user goal forbids file operations (mirrors local-ace forbidsFileOperations)
bool ForbidsFileOperations(const std::string& goal);

// Check if user goal requires HTML/CSS only (mirrors local-ace requiresHtmlCssOnly)
bool RequiresHtmlCssOnly(const std::string& goal);

// Check if user goal forbids JavaScript (mirrors local-ace forbidsJavaScript)
bool ForbidsJavaScript(const std::string& goal);

// Check if user goal forbids Markdown (mirrors local-ace forbidsMarkdown)
bool ForbidsMarkdown(const std::string& goal);

// Build compact constraints from user goal (mirrors local-ace buildCompactConstraints)
std::vector<std::string> BuildCompactConstraints(const std::string& goal);

// Check if validation is enabled (mirrors local-ace isValidationEnabled)
// Controlled by AGENT_DISABLE_VALIDATION env var.
bool IsValidationEnabled();

// Windows-specific: check for Unix commands on Windows PowerShell
void CheckUnixCommandsOnWindows(
    const std::vector<ContentBlock>& toolUseBlocks,
    LocalValidationResult& result);

}  // namespace core
}  // namespace agent
