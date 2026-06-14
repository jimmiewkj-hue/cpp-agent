#include "compact/AutoCompact.h"
#include "infra/EnvUtil.h"
#include "infra/StringUtil.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace agent {
namespace compact {

namespace {

using infra::GetEnvString;
using infra::IsTruthyEnvValue;

}  // namespace

// ============================================================================
// GetEffectiveContextWindowSize ? aligned with local-ace
// ============================================================================
int GetEffectiveContextWindowSize(int contextWindow, int maxOutputTokens) {
  int reservedTokensForSummary = std::min(
      maxOutputTokens,
      AutoCompactConfig::kMaxOutputTokensForSummary);

  // Allow env override for testing
  const std::string envWindow = GetEnvString("CPP_AGENT_AUTO_COMPACT_WINDOW");
  if (!envWindow.empty()) {
    int parsed = std::atoi(envWindow.c_str());
    if (parsed > 0) {
      contextWindow = std::min(contextWindow, parsed);
    }
  }

  return contextWindow - reservedTokensForSummary;
}

// ============================================================================
// GetAutoCompactThreshold ? aligned with local-ace
// ============================================================================
int GetAutoCompactThreshold(int effectiveWindow) {
  int threshold = effectiveWindow - AutoCompactConfig::kAutocompactBufferTokens;

  // Allow percentage override for testing
  const std::string envPct = GetEnvString("CPP_AGENT_AUTOCOMPACT_PCT_OVERRIDE");
  if (!envPct.empty()) {
    double pct = std::atof(envPct.c_str());
    if (pct > 0 && pct <= 100) {
      int pctThreshold = static_cast<int>(effectiveWindow * (pct / 100.0));
      threshold = std::min(pctThreshold, threshold);
    }
  }

  return threshold;
}

// ============================================================================
// CalculateTokenWarningState ? aligned with local-ace
// ============================================================================
TokenWarningState CalculateTokenWarningState(
    int tokenUsage, int contextWindow, int maxOutputTokens) {
  TokenWarningState state;

  int effectiveWindow = GetEffectiveContextWindowSize(contextWindow, maxOutputTokens);
  int autoCompactThreshold = GetAutoCompactThreshold(effectiveWindow);
  int threshold = IsAutoCompactEnabled() ? autoCompactThreshold : effectiveWindow;

  if (threshold > 0) {
    state.percentLeft = std::max(0,
        static_cast<int>((static_cast<double>(threshold - tokenUsage) / threshold) * 100));
  }

  int warningThreshold = threshold - AutoCompactConfig::kWarningThresholdBufferTokens;
  int errorThreshold = threshold - AutoCompactConfig::kErrorThresholdBufferTokens;

  state.isAboveWarningThreshold = tokenUsage >= warningThreshold;
  state.isAboveErrorThreshold = tokenUsage >= errorThreshold;
  state.isAboveAutoCompactThreshold =
      IsAutoCompactEnabled() && tokenUsage >= autoCompactThreshold;

  int actualContextWindow = GetEffectiveContextWindowSize(contextWindow, maxOutputTokens);
  int blockingLimit = actualContextWindow - AutoCompactConfig::kManualCompactBufferTokens;

  // Allow blocking limit override
  const std::string envBlocking = GetEnvString("CPP_AGENT_BLOCKING_LIMIT_OVERRIDE");
  if (!envBlocking.empty()) {
    int parsed = std::atoi(envBlocking.c_str());
    if (parsed > 0) blockingLimit = parsed;
  }

  state.isAtBlockingLimit = tokenUsage >= blockingLimit;

  return state;
}

// ============================================================================
// IsAutoCompactEnabled ? aligned with local-ace
// ============================================================================
bool IsAutoCompactEnabled() {
  if (IsTruthyEnvValue(GetEnvString("CPP_AGENT_DISABLE_COMPACT"))) {
    return false;
  }
  if (IsTruthyEnvValue(GetEnvString("CPP_AGENT_DISABLE_AUTO_COMPACT"))) {
    return false;
  }
  return true;  // Default: enabled
}

// ============================================================================
// ShouldAutoCompact ? aligned with local-ace
// ============================================================================
bool ShouldAutoCompact(int tokenCount, int contextWindow, int maxOutputTokens) {
  if (!IsAutoCompactEnabled()) return false;

  int threshold = GetAutoCompactThreshold(
      GetEffectiveContextWindowSize(contextWindow, maxOutputTokens));

  TokenWarningState state = CalculateTokenWarningState(
      tokenCount, contextWindow, maxOutputTokens);

  return state.isAboveAutoCompactThreshold;
}

// ============================================================================
// AutoCompactIfNeeded ? aligned with local-ace
// ============================================================================
AutoCompactDecision AutoCompactIfNeeded(
    int tokenCount,
    int contextWindow,
    int maxOutputTokens,
    AutoCompactTrackingState* tracking) {
  AutoCompactDecision decision;

  if (!IsAutoCompactEnabled()) return decision;

  // Circuit breaker: stop retrying after N consecutive failures
  // Mirrors local-ace BQ 2026-03-10 fix
  if (tracking && tracking->consecutiveFailures >=
      AutoCompactConfig::kMaxConsecutiveAutocompactFailures) {
    decision.circuitBreakerTripped = true;
    decision.consecutiveFailures = tracking->consecutiveFailures;
    return decision;
  }

  if (!ShouldAutoCompact(tokenCount, contextWindow, maxOutputTokens)) {
    return decision;
  }

  // Try compaction
  // Note: actual compactConversation call requires LLM backend which we
  // don't have in test. This function returns the DECISION to compact.
  // The caller (QueryLoop) will use this to trigger actual compaction.
  decision.wasCompacted = true;

  // On success, reset failure count
  if (tracking) {
    tracking->consecutiveFailures = 0;
    tracking->compacted = true;
    ++tracking->turnCounter;
  }

  return decision;
}

}  // namespace compact
}  // namespace agent
