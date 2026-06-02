#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <optional>

namespace agent {
namespace compact {

// P0-03: Micro-compact engine (aligned with local-ace microCompact.ts, 19KB).
// Token-aware message-level compaction: estimates token counts, evaluates
// time-based triggers, and clears compactable tool results to stay within
// context window limits without invalidating prompt cache.

// ============================================================================
// Microcompact config
// ============================================================================
struct MicroCompactConfig {
  bool enabled = true;
  bool cachedMCEnabled = false;       // Cache-editing API path (ant-only)
  int triggerThreshold = 20;          // Compact when tool results exceed this
  int keepRecent = 5;                 // Keep N most recent compactable results
  int gapThresholdMinutes = 60;       // Time-based trigger: gap since last asst msg
  std::vector<std::string> compactableTools = {
    "Read", "Glob", "Grep", "WebSearch", "WebFetch",
    "Bash", "TaskList", "TaskGet", "ListMcpResources", "ReadMcpResource"
  };
};

// ============================================================================
// Microcompact result (aligned with local-ace MicrocompactResult)
// ============================================================================
struct MicroCompactResult {
  std::vector<std::string> messages;  // Processed messages
  bool wasCompacted = false;
  int toolsCleared = 0;
  int tokensSaved = 0;
  std::string boundaryText;           // Boundary message to insert
};

// ============================================================================
// Token estimation (aligned with local-ace estimateMessageTokens)
// ============================================================================

// Rough token count: 4 characters ? 1 token, padded by 4/3
inline int RoughTokenCount(const std::string& text) {
  if (text.empty()) return 0;
  int raw = static_cast<int>(text.size()) / 4;
  return (raw * 4 + 2) / 3;  // Ceil division by 4/3
}

// Estimate tokens for a tool result block
int EstimateToolResultTokens(const std::string& content, bool isError);

// Estimate tokens for a tool use block
int EstimateToolUseTokens(const std::string& toolName,
                           const std::string& inputJson);

// Estimate total tokens across a batch of messages
int EstimateMessageTokens(const std::vector<std::string>& messages);

// ============================================================================
// Core microcompact functions
// ============================================================================

// Main microcompact entry point: evaluate triggers, clear old tool results,
// and return processed messages with optional boundary marker.
// (aligned with local-ace microcompactMessages)
MicroCompactResult MicroCompactMessages(
    const std::vector<std::string>& messages,
    const MicroCompactConfig& config,
    long long lastAssistantTimestampMs = 0);

// Time-based trigger: when the gap since the last assistant message exceeds
// the configured threshold, content-clear all but the most recent N
// compactable tool results. Returns nullopt when trigger doesn't fire.
// (aligned with local-ace maybeTimeBasedMicrocompact)
std::optional<MicroCompactResult> MaybeTimeBasedMicroCompact(
    const std::vector<std::string>& messages,
    const MicroCompactConfig& config,
    long long lastAssistantTimestampMs,
    long long nowMs);

// Content-clear compactable tool results: replace full result content
// with a compact placeholder, preserving only the most recent N.
// (aligned with local-ace contentClearToolResults)
int ClearCompactableToolResults(
    std::vector<std::string>& messages,
    const std::vector<std::string>& compactableToolNames,
    int keepRecent);

// Collect tool_use IDs whose tool name is in the compactable set.
// (aligned with local-ace collectCompactableToolIds)
std::vector<std::string> CollectCompactableToolIds(
    const std::vector<std::string>& messages,
    const std::vector<std::string>& compactableToolNames);

// Build a microcompact boundary message
std::string BuildMicroCompactBoundary(int toolsCleared, int tokensSaved);

// Check if a string contains a tool_use block for a compactable tool
bool IsCompactableToolUse(const std::string& message,
                           const std::vector<std::string>& compactableToolNames);

// Check if a string contains a tool_result block
bool IsToolResult(const std::string& message);

}  // namespace compact
}  // namespace agent
