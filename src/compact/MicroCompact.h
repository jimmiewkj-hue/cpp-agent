#pragma once

#include <algorithm>
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

// Rough token count: Unicode-aware estimation aligned with QueryLoop::EstimateTokens.
// ASCII ~4 chars/token, CJK/multi-byte ~1.5 tokens/char.
inline int RoughTokenCount(const std::string& text) {
  if (text.empty()) return 0;
  int tokens = 0;
  int asciiRun = 0;
  for (std::size_t i = 0; i < text.size(); ) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    if (ch < 0x80) {
      ++asciiRun; ++i;
    } else {
      if (asciiRun > 0) {
        tokens += asciiRun / 4;
        if (asciiRun % 4 > 0) ++tokens;
        asciiRun = 0;
      }
      tokens += 2;  // ~1-2 tokens per CJK/non-ASCII char
      if ((ch & 0xE0) == 0xC0) { i += 2; }
      else if ((ch & 0xF0) == 0xE0) { i += 3; }
      else if ((ch & 0xF8) == 0xF0) { i += 4; }
      else { ++i; }
    }
  }
  if (asciiRun > 0) {
    tokens += asciiRun / 4;
    if (asciiRun % 4 > 0) ++tokens;
  }
  return std::max(1, tokens);
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
