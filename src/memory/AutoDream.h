#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace agent {
namespace memory {
class MemoryIndex;
}
namespace infra {
class ProcessRunner;
}
namespace agents {
class SubAgentManager;
}

namespace memory {

// P0-03: Aligned with local-ace DEFAULTS (minHours=24, minSessions=5)
struct AutoDreamConfig {
  int minHours = 24;
  int minSessions = 5;
  int scanThrottleMs = 10 * 60 * 1000;  // 10-minute scan throttle
  // P1-3: Distill phase scheduling (separate from regular dream cycle)
  int distillMinDays = 30;        // minimum days between distill runs
  int distillMinSessions = 50;    // minimum sessions between distill runs
};

struct AutoDreamState {
  long long lastConsolidatedAtMs = 0;
  long long lastScanAtMs = 0;
  long long lastDistilledAtMs = 0;  // P1-3: last distill phase timestamp
  int sessionsSinceDistill = 0;      // P1-3: session counter for distill gate
  bool enabled = true;

  AutoDreamState() {
    lastConsolidatedAtMs = 0;
    lastScanAtMs = 0;
  }
};

// P0-03: Dream task state tracking (aligned with local-ace DreamTask)
enum class DreamTaskStatus { Idle, Running, Completed, Killed, Failed };

struct DreamTaskState {
  std::string taskId;
  DreamTaskStatus status = DreamTaskStatus::Idle;
  int sessionsReviewing = 0;
  long long priorMtimeMs = 0;
  long long startedAtMs = 0;
  std::vector<std::string> filesTouched;
};

class AutoDreamEngine {
 public:
  AutoDreamEngine(MemoryIndex* memoryIndex,
                  agents::SubAgentManager* subAgentManager);

  void Configure(const AutoDreamConfig& config);
  void Disable();
  void Enable();
  bool IsEnabled() const;

  bool ShouldExecute();
  bool Execute();

  bool IsGateOpen() const;
  bool IsTimeGatePassed() const;
  bool IsSessionGatePassed();

  // P0-03: Task lifecycle (aligned with local-ace DreamTask)
  std::string RegisterDreamTask(const DreamTaskState& task);
  void CompleteDreamTask(const std::string& taskId);
  void FailDreamTask(const std::string& taskId);
  const DreamTaskState* GetDreamTask(const std::string& taskId) const;

  // P0-03: Consolidation prompt builder (aligned with local-ace buildConsolidationPrompt)
  std::string BuildConsolidationPrompt(const std::string& extra) const;

  AutoDreamState state() const;

 private:
  bool AcquireLock();
  void ReleaseLock();
  bool TryAcquireLock(long long* priorMtimeMs);
  bool IsLockExpired() const;
  std::string LockFilePath() const;
  long long ReadLastConsolidatedAt() const;
  void RollbackConsolidationLock(long long priorMtimeMs);

  bool RunOrientPhase(std::string* context);
  bool RunGatherPhase(std::string* context);
  bool RunConsolidatePhase(const std::string& context);
  bool RunPrunePhase();
  // P1-3: Distill phase — skeleton for pattern extraction from historical
  // sessions. Currently a placeholder; pattern mining logic to be iterated.
  bool RunDistillPhase();

  // P0-03: Session listing (aligned with local-ace listSessionsTouchedSince)
  std::vector<std::string> ListSessionsTouchedSince(long long sinceMs) const;

  MemoryIndex* memoryIndex_;
  agents::SubAgentManager* subAgentManager_;
  AutoDreamConfig config_;
  AutoDreamState state_;

  // P0-03: Dream task tracking
  std::map<std::string, DreamTaskState> dreamTasks_;
};

}  // namespace memory
}  // namespace agent
