#pragma once

// P2-3: Validator effectiveness visualization report.
// Generates a structured summary of validator interventions at session end.
// Pure observability — no side effects on loop behavior.
// Controlled by CPP_AGENT_VALIDATOR_REPORT env var (default: "1" = enabled).

#include "core/QueryLoop.h"

#include <string>
#include <vector>

namespace agent {
namespace core {

// Structured report data for validator effectiveness.
struct ValidatorReportData {
  int totalInterventions = 0;     // total validator retry requests
  int totalSamples = 0;           // outcomes recorded (sliding window cap 20)
  int accepted = 0;               // main model changed behavior after guidance
  int rejected = 0;               // main model ignored guidance
  double effectivenessRatio = 1.0; // accepted / totalSamples (1.0 if < 10 samples)
  int totalValidatorRetries = 0;  // session-wide retry count (never resets)
  int validatorNudges = 0;        // forced nudge count at retry limit
  bool downgraded = false;        // tier auto-downgraded due to low effectiveness
  std::string summary;            // human-readable one-liner
};

// Build the report from the final query loop state.
// Does not throw; returns a zeroed report if no validator data exists.
ValidatorReportData BuildValidatorReport(
    const QueryLoopInternalState& state);

// Format the report as a concise human-readable string.
// Suitable for logging and/or injecting into the session transcript.
std::string FormatValidatorReport(const ValidatorReportData& report);

// Check if the validator report feature is enabled.
// Reads CPP_AGENT_VALIDATOR_REPORT env var (default "1" = enabled).
bool IsValidatorReportEnabled();

// Emit the validator report: log it and optionally append to session manager
// transcript. Called once at the end of RunFull().
void EmitValidatorReport(
    const QueryLoopInternalState& state,
    infra::SessionManager* sessionManager);

}  // namespace core
}  // namespace agent
