#pragma once

#include "agents/SubAgentManager.h"
#include "core/AgentTypes.h"
#include "core/StateTypes.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace agent {
namespace infra {

struct SessionSnapshot {
  int formatVersion = 3;
  std::string sessionId;
  std::string timestamp;
  std::vector<core::Message> messages;
  std::vector<agents::SubAgentTaskLifecycle> subAgentTasks;
  std::vector<agents::SubAgentExecutorSlot> subAgentExecutors;
  core::SessionMetadata metadata;
};

class SessionManager {
 public:
  SessionManager(const std::string& sessionDir);

  void SetMessages(const std::vector<core::Message>& messages);
  std::vector<core::Message> messages() const;
  void AppendMessage(const core::Message& message);

  void SetMetadata(const core::SessionMetadata& metadata);
  core::SessionMetadata metadata() const;
  void SetSubAgentTasks(
      const std::vector<agents::SubAgentTaskLifecycle>& subAgentTasks);
  void UpsertSubAgentTask(const agents::SubAgentTaskLifecycle& task);
  std::vector<agents::SubAgentTaskLifecycle> subAgentTasks() const;
  void SetSubAgentExecutors(
      const std::vector<agents::SubAgentExecutorSlot>& executors);
  std::vector<agents::SubAgentExecutorSlot> subAgentExecutors() const;

  void PersistSnapshot() const;
  bool RestoreFromDisk();

  std::string LatestTranscriptPath() const;
  std::string TranscriptJsonlPath() const;
  std::string SnapshotPath() const;
  std::string LegacyBinarySnapshotPath() const;
  std::string LegacySnapshotPath() const;

  void AppendTranscriptLine(const std::string& jsonLine);
  void AppendMessageToTranscript(const core::Message& message);
  void FlushTranscriptBuffer();

 private:
  SessionSnapshot BuildSnapshot() const;

  mutable std::mutex mutex_;
  mutable std::mutex transcriptMutex_;
  std::string sessionDir_;
  std::vector<core::Message> messages_;
  std::vector<agents::SubAgentTaskLifecycle> subAgentTasks_;
  std::vector<agents::SubAgentExecutorSlot> subAgentExecutors_;
  core::SessionMetadata metadata_;
  std::vector<std::string> transcriptBuffer_;
};

}  // namespace infra
}  // namespace agent
