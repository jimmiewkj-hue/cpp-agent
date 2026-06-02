#pragma once

#include <string>
#include <vector>
#include <map>

namespace agent {
namespace coordinator {

// P0-03: Coordinator mode (aligned with local-ace coordinatorMode.ts).
// Manages agent swarm coordination: worker spawning, task lifecycle,
// system prompts, and tool context for the coordinator role.

struct CoordinatorConfig {
  bool enabled = false;
  std::string scratchpadDir;
  std::vector<std::string> mcpServerNames;
  bool simpleMode = false;  // If true, workers only get Bash/Read/Edit
};

// Check if coordinator mode is active (reads CLAUDE_CODE_COORDINATOR_MODE env).
bool IsCoordinatorMode();

// Match stored session mode with current mode. Returns a warning string if
// mismatch was detected and corrected, or empty string if modes match.
// Stores mode in env var for the resumed session.
std::string MatchSessionMode(const std::string& sessionMode);

// Build the coordinator system prompt (aligned with local-ace).
// Includes role description, tool usage, worker management, task workflow,
// concurrency rules, and verification standards.
std::string BuildCoordinatorSystemPrompt(const CoordinatorConfig& config);

// Build worker tools context for injection into user/system prompts.
// Lists available tools, MCP server access, and scratchpad directory.
std::map<std::string, std::string> BuildCoordinatorUserContext(
    const CoordinatorConfig& config);

// Get the set of tools that are internal worker-only (not delegated).
std::vector<std::string> GetInternalWorkerTools();

// Get the standard worker tool set (tools available to worker sub-agents).
std::vector<std::string> GetWorkerTools(bool simpleMode = false);

// Build a task notification XML for worker completion/failure.
std::string BuildTaskNotification(const std::string& taskId,
                                   const std::string& status,
                                   const std::string& summary,
                                   const std::string& result,
                                   int totalTokens,
                                   int toolUses,
                                   long long durationMs);

// Parse a task notification XML to extract task status.
struct TaskNotification {
  std::string taskId;
  std::string status;       // completed, failed, killed
  std::string summary;
  std::string result;
  int totalTokens = 0;
  int toolUses = 0;
  long long durationMs = 0;
};
TaskNotification ParseTaskNotification(const std::string& xml);

// Coordination phase tracking (Research -> Synthesis -> Implementation -> Verify)
enum class CoordinationPhase {
  Research,
  Synthesis,
  Implementation,
  Verification
};

const char* CoordinationPhaseToString(CoordinationPhase phase);

}  // namespace coordinator
}  // namespace agent
