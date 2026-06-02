#pragma once

#include <string>

namespace agent {
namespace compact {

// P0-03: Time-based microcompact configuration (aligned with local-ace timeBasedMCConfig).
// When the gap between the last assistant message and now exceeds the threshold,
// old tool results are cleared before the next ModelCall since the server-side
// prompt cache has expired and the full prefix will be rewritten anyway.

struct TimeBasedMCConfig {
  bool enabled = false;
  int gapThresholdMinutes = 60;   // 60 min = cache TTL expired for all users
  int keepRecent = 5;             // Keep this many most-recent compactable tool results
};

// Reads config from environment variables:
//   AGENT_TC_MC_ENABLED=1
//   AGENT_TC_MC_GAP_MINUTES=60
//   AGENT_TC_MC_KEEP_RECENT=5
TimeBasedMCConfig GetTimeBasedMCConfig();

// Returns true if time-based microcompact should trigger.
// Compares current time against the timestamp of the last assistant message.
bool ShouldTriggerTimeBasedMC(const TimeBasedMCConfig& config,
                               long long lastAssistantTimestampMs,
                               long long nowMs);

}  // namespace compact
}  // namespace agent
