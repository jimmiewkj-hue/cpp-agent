#include "core/WriterSubAgent.h"

#include "infra/EnvUtil.h"
#include "infra/Logger.h"
#include "infra/ThreadPool.h"
#include "third_party/nlohmann_json.hpp"

#include <windows.h>

#include <chrono>
#include <fstream>
#include <sstream>

namespace agent {
namespace core {

bool IsWriterEnabled() {
  return infra::GetEnvBool("CPP_AGENT_WRITER", false);
}

// Build an ISO 8601 timestamp string (UTC).
static std::string BuildIsoTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto timeT = std::chrono::system_clock::to_time_t(now);
  struct tm utc;
  gmtime_s(&utc, &timeT);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
  return std::string(buf);
}

// Serialize the 11-field state to JSON.
static std::string SerializeState(const WriterSessionState& state) {
  nlohmann::json j;
  j["session_id"] = state.sessionId;
  j["turn_count"] = state.turnCount;
  j["model"] = state.model;
  j["intent"] = state.intent;
  j["progress"] = state.progress;
  j["files_involved"] = state.filesInvolved;
  j["errors"] = state.errors;
  j["next_steps"] = state.nextSteps;
  j["context_usage_percent"] = state.contextUsagePercent;
  j["checkpoint_phase"] = state.checkpointPhase;
  j["updated_at"] = state.updatedAt;
  return j.dump(2);
}

// Perform the actual file write (may run on a worker thread).
static bool DoWriteFile(const std::string& content,
                        const std::string& filePath) {
  std::ofstream ofs(filePath, std::ios::trunc | std::ios::out);
  if (!ofs.is_open()) return false;
  ofs << content;
  ofs.close();
  return ofs.good() || !ofs.fail();
}

bool WriteSessionStateFile(const WriterSessionState& state,
                           const std::string& sessionDir) {
  if (sessionDir.empty()) return false;

  // Fill in timestamp if not set
  WriterSessionState stateCopy = state;
  if (stateCopy.updatedAt.empty()) {
    stateCopy.updatedAt = BuildIsoTimestamp();
  }

  const std::string jsonContent = SerializeState(stateCopy);
  const std::string filePath = sessionDir + "/session-state.json";

  // Try async write via ThreadPool, fall back to synchronous
  try {
    infra::ThreadPool::Global().Submit(
        [jsonContent, filePath]() {
          DoWriteFile(jsonContent, filePath);
        },
        infra::TaskPriority::LOW);
    LOG_INFO(QUERY, "Writer SubAgent: async write submitted",
             {{"path", filePath}});
    return true;
  } catch (const std::exception&) {
    // ThreadPool unavailable — synchronous fallback
    bool ok = DoWriteFile(jsonContent, filePath);
    if (ok) {
      LOG_INFO(QUERY, "Writer SubAgent: sync write completed",
               {{"path", filePath}});
    } else {
      LOG_WARN(QUERY, "Writer SubAgent: write failed",
               {{"path", filePath}});
    }
    return ok;
  }
}

}  // namespace core
}  // namespace agent
