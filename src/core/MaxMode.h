#pragma once

// P1-2: Max Mode — parallel sampling with main-model judge.
// Aligned with MiMo Code's Max Mode (parallel candidates). At critical
// decision points during the first N turns, the system fires 2-3 parallel
// side queries with high temperature to generate diverse solution
// candidates. The main model then judges which candidate is best.
//
// Design:
//   - Uses ThreadPool for parallel sampling (all queries fire simultaneously)
//   - Uses SideQueryClient (no main-model token cost for sampling)
//   - Only fires at decision points (first N turns + critical patterns)
//   - Candidate responses are injected as a meta message for the main model
//
// Switch: CPP_AGENT_MAX_MODE=1 (default OFF, costs 2-3x side-query tokens)
// Config: CPP_AGENT_MAX_MODE_SAMPLES (default 2, max 3)
//         CPP_AGENT_MAX_MODE_TURNS (default 10, max turns to activate)

#include "core/AgentTypes.h"

#include <string>
#include <vector>

namespace agent {
namespace api { class SideQueryClient; }
namespace core {

struct MaxModeCandidate {
  std::string response;     // the sampled text response
  double temperature = 1.0; // temperature used for this sample
  int sampleIndex = 0;      // 0, 1, 2...
};

struct MaxModeResult {
  bool activated = false;             // whether max mode was triggered
  std::vector<MaxModeCandidate> candidates;
  std::string judgePrompt;            // the meta-message injected for judging
};

// Check if Max Mode is enabled and should fire for this turn.
bool ShouldActivateMaxMode(int turnCount, int modelCallCount);

// Run parallel sampling and build the judge prompt.
// This is a blocking call (waits for all side queries to complete).
// Returns a MaxModeResult with candidates and the judge meta-message.
MaxModeResult RunMaxModeSampling(
    const std::string& userGoal,
    const std::string& currentContext,
    api::SideQueryClient& sideQueryClient,
    const std::string& model);

}  // namespace core
}  // namespace agent
