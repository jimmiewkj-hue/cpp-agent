#pragma once

#include "core/AgentTypes.h"
#include "tools/ToolOrchestrator.h"

#include <functional>
#include <string>
#include <vector>

namespace agent {
namespace core {

enum class ToolExecStatus {
  Queued,
  Executing,
  Completed,
  Yielded
};

struct TrackedTool {
  std::string id;
  ContentBlock block;
  ToolExecStatus status = ToolExecStatus::Queued;
  bool isConcurrencySafe = false;
  ContentBlock result;
  bool resultReady = false;
};

class StreamingToolExecutor {
 public:
  StreamingToolExecutor(tools::ToolOrchestrator& orchestrator,
                        const std::vector<Message>& messages);

  void AddTool(const ContentBlock& toolUse);
  bool HasPendingTools() const;
  bool HasCompletedTools() const;

  std::vector<ContentBlock> YieldCompletedResults();
  void ExecutePending();

  void Discard();

 private:
  tools::ToolOrchestrator& orchestrator_;
  const std::vector<Message>& messages_;
  std::vector<TrackedTool> tools_;
  bool hasErrored_ = false;
  bool discarded_ = false;
};

}  // namespace core
}  // namespace agent
