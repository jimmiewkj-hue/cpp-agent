#pragma once

// P0-3: Writer SubAgent — asynchronous session state file writer.
// Aligned with MiMo Code's Writer mechanism. When a checkpoint is generated,
// the Writer writes a structured 11-field status file that external tools
// (TUI, dashboards, other agents) can consume without querying the LLM.
//
// Design:
//   - Uses ThreadPool for async I/O (non-blocking to main loop)
//   - Falls back to synchronous write if ThreadPool is unavailable
//   - File format: JSON with 11 fixed fields (idempotent overwrite)
//   - No LLM tokens consumed (pure local serialization)
//
// Switch: CPP_AGENT_WRITER=1 (default off, zero cost when disabled)

#include <string>
#include <vector>

namespace agent {
namespace core {

// Fixed 11-field session state structure written by the Writer.
struct WriterSessionState {
  std::string sessionId;
  int turnCount = 0;
  std::string model;
  std::string intent;           // from checkpoint summary
  std::string progress;         // from checkpoint summary
  std::string filesInvolved;    // from checkpoint summary
  std::string errors;           // from checkpoint summary
  std::string nextSteps;        // from checkpoint summary
  int contextUsagePercent = 0;
  int checkpointPhase = 0;
  std::string updatedAt;        // ISO timestamp
};

// Write the session state file asynchronously (or synchronously as fallback).
// The file is written to: <sessionDir>/session-state.json
// Returns true if the write was initiated/completed successfully.
bool WriteSessionStateFile(const WriterSessionState& state,
                           const std::string& sessionDir);

// Check if the Writer feature is enabled via env var.
bool IsWriterEnabled();

}  // namespace core
}  // namespace agent
