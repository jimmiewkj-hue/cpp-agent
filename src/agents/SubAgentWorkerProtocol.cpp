#include "agents/SubAgentWorkerProtocol.h"

#include "infra/ProtoLite.h"

namespace agent {
namespace agents {

namespace {

enum WorkerFieldTag {
  kWorkerTaskId = 1,
  kWorkerExecutorId = 2,
  kWorkerPrompt = 3,
  kWorkerPriority = 4,
  kWorkerCheckpointId = 5,
  kWorkerResumeCursor = 6,
  kWorkerState = 7,
  kWorkerCompletedUnits = 8,
  kWorkerTotalUnits = 9,
  kWorkerSummary = 10,
  kWorkerUpdatedAt = 11
};

}  // namespace

std::string SerializeWorkerRequest(const SubAgentWorkerRequest& request) {
  std::string output;
  infra::protolite::WriteString(&output, kWorkerTaskId, request.taskId);
  infra::protolite::WriteString(&output, kWorkerExecutorId, request.executorId);
  infra::protolite::WriteString(&output, kWorkerPrompt, request.prompt);
  infra::protolite::WriteInt32(&output, kWorkerPriority, request.priority);
  infra::protolite::WriteString(
      &output, kWorkerCheckpointId, request.checkpointId);
  infra::protolite::WriteString(
      &output, kWorkerResumeCursor, request.resumeCursor);
  return output;
}

bool DeserializeWorkerRequest(
    const std::string& bytes,
    SubAgentWorkerRequest* request) {
  if (request == nullptr) {
    return false;
  }
  infra::protolite::Reader reader(bytes);
  infra::protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kWorkerTaskId:
        infra::protolite::FieldToString(field, &request->taskId);
        break;
      case kWorkerExecutorId:
        infra::protolite::FieldToString(field, &request->executorId);
        break;
      case kWorkerPrompt:
        infra::protolite::FieldToString(field, &request->prompt);
        break;
      case kWorkerPriority:
        infra::protolite::FieldToInt32(field, &request->priority);
        break;
      case kWorkerCheckpointId:
        infra::protolite::FieldToString(field, &request->checkpointId);
        break;
      case kWorkerResumeCursor:
        infra::protolite::FieldToString(field, &request->resumeCursor);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

std::string SerializeWorkerStatus(const SubAgentWorkerStatus& status) {
  std::string output;
  infra::protolite::WriteString(&output, kWorkerTaskId, status.taskId);
  infra::protolite::WriteInt32(
      &output, kWorkerState, static_cast<int>(status.state));
  infra::protolite::WriteInt32(
      &output, kWorkerCompletedUnits, status.completedUnits);
  infra::protolite::WriteInt32(&output, kWorkerTotalUnits, status.totalUnits);
  infra::protolite::WriteString(
      &output, kWorkerCheckpointId, status.checkpointId);
  infra::protolite::WriteString(
      &output, kWorkerResumeCursor, status.resumeCursor);
  infra::protolite::WriteString(&output, kWorkerSummary, status.summary);
  infra::protolite::WriteInt64(&output, kWorkerUpdatedAt, status.updatedAtUnixMs);
  return output;
}

bool DeserializeWorkerStatus(
    const std::string& bytes,
    SubAgentWorkerStatus* status) {
  if (status == nullptr) {
    return false;
  }
  infra::protolite::Reader reader(bytes);
  infra::protolite::Field field;
  while (reader.ReadField(&field)) {
    switch (field.number) {
      case kWorkerTaskId:
        infra::protolite::FieldToString(field, &status->taskId);
        break;
      case kWorkerState: {
        int stateValue = 0;
        infra::protolite::FieldToInt32(field, &stateValue);
        status->state = static_cast<WorkerTaskState>(stateValue);
        break;
      }
      case kWorkerCompletedUnits:
        infra::protolite::FieldToInt32(field, &status->completedUnits);
        break;
      case kWorkerTotalUnits:
        infra::protolite::FieldToInt32(field, &status->totalUnits);
        break;
      case kWorkerCheckpointId:
        infra::protolite::FieldToString(field, &status->checkpointId);
        break;
      case kWorkerResumeCursor:
        infra::protolite::FieldToString(field, &status->resumeCursor);
        break;
      case kWorkerSummary:
        infra::protolite::FieldToString(field, &status->summary);
        break;
      case kWorkerUpdatedAt:
        infra::protolite::FieldToInt64(field, &status->updatedAtUnixMs);
        break;
      default:
        break;
    }
  }
  return reader.ok();
}

}  // namespace agents
}  // namespace agent
