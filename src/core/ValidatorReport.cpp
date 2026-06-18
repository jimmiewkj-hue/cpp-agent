// P2-3: Validator effectiveness visualization report.
// Pure observability — reads state, produces report, no mutation.

#include "core/ValidatorReport.h"

#include "infra/EnvUtil.h"
#include "infra/Logger.h"
#include "infra/SessionManager.h"

#include <sstream>

namespace agent {
namespace core {

bool IsValidatorReportEnabled() {
  // Default enabled: pure observability with no side effects.
  return infra::GetEnvBool("CPP_AGENT_VALIDATOR_REPORT", true);
}

ValidatorReportData BuildValidatorReport(
    const QueryLoopInternalState& state) {
  ValidatorReportData report;

  // Sliding window outcomes (capped at 20 in QueryLoop)
  const auto& outcomes = state.validatorOutcomes;
  report.totalSamples = static_cast<int>(outcomes.size());

  for (const auto& o : outcomes) {
    if (o.resolved) {
      ++report.accepted;
    } else {
      ++report.rejected;
    }
  }

  // Session-wide counters
  report.totalValidatorRetries = state.totalValidatorRetryCount;
  report.validatorNudges = state.validatorNudgeCount;

  // Effectiveness ratio (mirrors ComputeValidatorEffectiveness in QueryLoop.h)
  if (report.totalSamples >= 10) {
    report.effectivenessRatio =
        static_cast<double>(report.accepted) /
        static_cast<double>(report.totalSamples);
  } else {
    report.effectivenessRatio = 1.0;  // insufficient data
  }

  // Total interventions = retries requested (not just accepted ones)
  report.totalInterventions = report.totalValidatorRetries;

  // Detect downgrade: effectiveness below 0.3 with enough samples
  report.downgraded =
      (report.totalSamples >= 10 && report.effectivenessRatio < 0.3);

  // Build human-readable summary
  std::ostringstream ss;
  if (report.totalSamples == 0 && report.totalValidatorRetries == 0) {
    ss << "Validator: no interventions this session.";
  } else {
    ss << "Validator: " << report.totalInterventions << " interventions, "
       << report.totalSamples << " tracked outcomes ("
       << report.accepted << " accepted, "
       << report.rejected << " rejected), "
       << "effectiveness=" << static_cast<int>(report.effectivenessRatio * 100) << "%";
    if (report.validatorNudges > 0) {
      ss << ", " << report.validatorNudges << " forced nudges";
    }
    if (report.downgraded) {
      ss << " [DOWNGRADED: below 30% threshold]";
    }
  }
  report.summary = ss.str();

  return report;
}

std::string FormatValidatorReport(const ValidatorReportData& report) {
  // Structured multi-line format for log / transcript
  std::ostringstream ss;
  ss << "=== Validator Effectiveness Report ===\n";
  ss << "  interventions:    " << report.totalInterventions << "\n";
  ss << "  tracked_outcomes: " << report.totalSamples << "\n";
  ss << "  accepted:         " << report.accepted << "\n";
  ss << "  rejected:         " << report.rejected << "\n";
  ss << "  effectiveness:    "
     << static_cast<int>(report.effectivenessRatio * 100) << "%\n";
  ss << "  total_retries:    " << report.totalValidatorRetries << "\n";
  ss << "  forced_nudges:    " << report.validatorNudges << "\n";
  ss << "  downgraded:       " << (report.downgraded ? "yes" : "no") << "\n";
  ss << "  summary: " << report.summary << "\n";
  ss << "======================================";
  return ss.str();
}

void EmitValidatorReport(
    const QueryLoopInternalState& state,
    infra::SessionManager* sessionManager) {
  if (!IsValidatorReportEnabled()) return;

  ValidatorReportData report = BuildValidatorReport(state);

  // Skip report if validator was never active
  if (report.totalSamples == 0 && report.totalValidatorRetries == 0) return;

  // Log structured entry (call Logger directly to avoid macro comma issues
  // with initializer-list pairs)
  ::agent::infra::Logger::Instance().Log(
      ::agent::infra::LogLevel::INFO,
      ::agent::infra::LogCategory::QUERY,
      report.summary,
      {{"interventions", std::to_string(report.totalInterventions)},
       {"tracked_outcomes", std::to_string(report.totalSamples)},
       {"accepted", std::to_string(report.accepted)},
       {"rejected", std::to_string(report.rejected)},
       {"effectiveness_pct",
        std::to_string(static_cast<int>(report.effectivenessRatio * 100))},
       {"total_retries", std::to_string(report.totalValidatorRetries)},
       {"forced_nudges", std::to_string(report.validatorNudges)},
       {"downgraded", report.downgraded ? "yes" : "no"}},
      __FILE__, __LINE__);

  // Append to session transcript for post-session analysis
  if (sessionManager) {
    std::string formatted = FormatValidatorReport(report);
    Message reportMsg;
    reportMsg.role = MessageRole::System;
    reportMsg.uuid = "validator-effectiveness-report";
    reportMsg.isMeta = true;
    reportMsg.content.push_back(ContentBlock::MakeText(formatted));
    sessionManager->AppendMessageToTranscript(reportMsg);
  }
}

}  // namespace core
}  // namespace agent
