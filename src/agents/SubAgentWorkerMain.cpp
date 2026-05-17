#include "agents/SubAgentWorkerProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return std::string();
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

bool WriteFile(const std::string& path, const std::string& content) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }
  output.write(content.data(), static_cast<std::streamsize>(content.size()));
  return output.good();
}

bool FileExists(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return input.good();
}

long long NowUnixMs() {
  return static_cast<long long>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

int ParseResumeCursor(const std::string& value) {
  if (value.empty()) {
    return 0;
  }
  return std::max(0, std::atoi(value.c_str()));
}

std::string BuildCheckpointId(
    const std::string& taskId,
    int completedUnits) {
  return taskId + "-cp-" + std::to_string(completedUnits);
}

}  // namespace

int main(int argc, char** argv) {
  std::string requestPath;
  std::string statusPath;
  std::string controlPath;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    if (key == "--request") {
      requestPath = value;
    } else if (key == "--status") {
      statusPath = value;
    } else if (key == "--control") {
      controlPath = value;
    }
  }

  if (requestPath.empty() || statusPath.empty() || controlPath.empty()) {
    return 2;
  }

  agent::agents::SubAgentWorkerRequest request;
  if (!agent::agents::DeserializeWorkerRequest(ReadFile(requestPath), &request) ||
      request.taskId.empty()) {
    return 2;
  }

  const int totalUnits =
      std::max(3, static_cast<int>(request.prompt.size() / 8) + 1);
  int completedUnits = ParseResumeCursor(request.resumeCursor);
  if (completedUnits > totalUnits) {
    completedUnits = totalUnits;
  }

  auto writeStatus = [&](agent::agents::WorkerTaskState state,
                         const std::string& summary) {
    agent::agents::SubAgentWorkerStatus status;
    status.taskId = request.taskId;
    status.state = state;
    status.completedUnits = completedUnits;
    status.totalUnits = totalUnits;
    status.resumeCursor = std::to_string(completedUnits);
    status.checkpointId = BuildCheckpointId(request.taskId, completedUnits);
    status.summary = summary;
    status.updatedAtUnixMs = NowUnixMs();
    return WriteFile(statusPath, agent::agents::SerializeWorkerStatus(status));
  };

  if (!writeStatus(agent::agents::WorkerTaskState::Running,
                   "Worker bootstrapped")) {
    return 2;
  }

  while (completedUnits < totalUnits) {
    if (FileExists(controlPath)) {
      writeStatus(
          agent::agents::WorkerTaskState::Preempted,
          "Worker preempted after persisting checkpoint");
      return 3;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    ++completedUnits;
    if (!writeStatus(
            agent::agents::WorkerTaskState::Running,
            "Processed slice " + std::to_string(completedUnits) + "/" +
                std::to_string(totalUnits))) {
      return 4;
    }
  }

  if (!writeStatus(
          agent::agents::WorkerTaskState::Completed,
          "Worker completed prompt execution")) {
    return 4;
  }
  return 0;
}
