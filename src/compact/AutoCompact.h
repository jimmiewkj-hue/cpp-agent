#pragma once

#include <string>
#include <vector>

namespace agent {
namespace compact {

// ============================================================================
// AutoCompact ? aligned with local-ace autoCompact.ts (351 lines)
// ============================================================================

// Mirror of local-ace AutoCompactTrackingState
struct AutoCompactTrackingState {
  bool compacted = false;
  int turnCounter = 0;
  std::string turnId;
  int consecutiveFailures = 0;  // Circuit breaker: reset on success
};

// Token budget constants (mirrors local-ace)
struct AutoCompactConfig {
  static constexpr int kMaxOutputTokensForSummary = 20000;
  static constexpr int kAutocompactBufferTokens = 13000;
  static constexpr int kWarningThresholdBufferTokens = 20000;
  static constexpr int kErrorThresholdBufferTokens = 20000;
  static constexpr int kManualCompactBufferTokens = 3000;
  static constexpr int kMaxConsecutiveAutocompactFailures = 3;
  static constexpr int kContextWindowDefault = 200000;
};

// Get the effective context window size (window - output tokens).
// Mirrors local-ace getEffectiveContextWindowSize.
int GetEffectiveContextWindowSize(int contextWindow, int maxOutputTokens);

// Get the auto-compact threshold (effective window - buffer).
// Mirrors local-ace getAutoCompactThreshold.
int GetAutoCompactThreshold(int effectiveWindow);

// Calculate token warning state from usage and model.
// Mirrors local-ace calculateTokenWarningState.
struct TokenWarningState {
  int percentLeft = 100;
  bool isAboveWarningThreshold = false;
  bool isAboveErrorThreshold = false;
  bool isAboveAutoCompactThreshold = false;
  bool isAtBlockingLimit = false;
};
TokenWarningState CalculateTokenWarningState(
    int tokenUsage, int contextWindow, int maxOutputTokens);

// Check if auto-compact is enabled.
// Mirrors local-ace isAutoCompactEnabled.
bool IsAutoCompactEnabled();

// Decide whether auto-compact should trigger.
// Mirrors local-ace shouldAutoCompact.
bool ShouldAutoCompact(int tokenCount, int contextWindow, int maxOutputTokens);

// Main entry point for auto-compact.
// Returns whether compaction occurred and updated tracking state.
// Mirrors local-ace autoCompactIfNeeded.
struct AutoCompactDecision {
  bool wasCompacted = false;
  int consecutiveFailures = 0;
  bool circuitBreakerTripped = false;
};
AutoCompactDecision AutoCompactIfNeeded(
    int tokenCount,
    int contextWindow,
    int maxOutputTokens,
    AutoCompactTrackingState* tracking);

}  // namespace compact
}  // namespace agent
