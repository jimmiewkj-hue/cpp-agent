#include "compact/TimeBasedMCConfig.h"

#include <cstdlib>
#include <chrono>

namespace agent {
namespace compact {

TimeBasedMCConfig GetTimeBasedMCConfig() {
  TimeBasedMCConfig config;

  const char* envEnabled = std::getenv("AGENT_TC_MC_ENABLED");
  if (envEnabled) {
    config.enabled = (std::atoi(envEnabled) != 0);
  }

  const char* envGap = std::getenv("AGENT_TC_MC_GAP_MINUTES");
  if (envGap) {
    int val = std::atoi(envGap);
    if (val > 0) config.gapThresholdMinutes = val;
  }

  const char* envKeep = std::getenv("AGENT_TC_MC_KEEP_RECENT");
  if (envKeep) {
    int val = std::atoi(envKeep);
    if (val > 0) config.keepRecent = val;
  }

  return config;
}

bool ShouldTriggerTimeBasedMC(const TimeBasedMCConfig& config,
                               long long lastAssistantTimestampMs,
                               long long nowMs) {
  if (!config.enabled) return false;
  if (lastAssistantTimestampMs <= 0) return false;

  long long gapMs = nowMs - lastAssistantTimestampMs;
  long long thresholdMs = static_cast<long long>(config.gapThresholdMinutes) * 60 * 1000;

  return gapMs >= thresholdMs;
}

}  // namespace compact
}  // namespace agent
