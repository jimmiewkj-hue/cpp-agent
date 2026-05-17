#pragma once

#include <chrono>
#include <functional>
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

struct AutoDreamConfig {
  int minHours = 24;
  int minSessions = 5;
  int scanThrottleMs = 10 * 60 * 1000;
};

struct AutoDreamState {
  long long lastConsolidatedAtMs = 0;
  long long lastScanAtMs = 0;
  bool enabled = true;

  AutoDreamState() {
    lastConsolidatedAtMs = 0;
    lastScanAtMs = 0;
  }
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

  AutoDreamState state() const;

 private:
  bool AcquireLock();
  void ReleaseLock();
  bool IsLockExpired() const;
  std::string LockFilePath() const;

  bool RunOrientPhase(std::string* context);
  bool RunGatherPhase(std::string* context);
  bool RunConsolidatePhase(const std::string& context);
  bool RunPrunePhase();

  MemoryIndex* memoryIndex_;
  agents::SubAgentManager* subAgentManager_;
  AutoDreamConfig config_;
  AutoDreamState state_;
};

}  // namespace memory
}  // namespace agent
