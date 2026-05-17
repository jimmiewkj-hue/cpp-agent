#pragma once

#include <string>

namespace agent {
namespace agents {

enum class WorkerTaskState {
  Running = 0,
  Completed = 1,
  Preempted = 2,
  Failed = 3
};

struct SubAgentWorkerRequest {
  std::string taskId;
  std::string executorId;
  std::string prompt;
  int priority = 0;
  std::string checkpointId;
  std::string resumeCursor;
};

struct SubAgentWorkerStatus {
  std::string taskId;
  WorkerTaskState state = WorkerTaskState::Running;
  int completedUnits = 0;
  int totalUnits = 0;
  std::string checkpointId;
  std::string resumeCursor;
  std::string summary;
  long long updatedAtUnixMs = 0;
};

std::string SerializeWorkerRequest(const SubAgentWorkerRequest& request);
bool DeserializeWorkerRequest(
    const std::string& bytes,
    SubAgentWorkerRequest* request);

std::string SerializeWorkerStatus(const SubAgentWorkerStatus& status);
bool DeserializeWorkerStatus(
    const std::string& bytes,
    SubAgentWorkerStatus* status);

}  // namespace agents
}  // namespace agent
